/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2011-2018 K. Lange
 * Copyright (C) 2012 Markus Schober
 * Copyright (C) 2015 Dale Weiler
 *
 * Processes
 *
 * Internal format format for a process and functions to spawn
 * new processes and manage the process tree.
 */
#include <kernel/system.h>
#include <kernel/process.h>
#include <kernel/bitset.h>
#include <kernel/logging.h>
#include <kernel/shm.h>
#include <kernel/printf.h>

#include <sys/wait.h>

#include <toaru/list.h>
#include <toaru/tree.h>

tree_t * process_tree;  /* Parent->Children tree */
list_t * process_list;  /* Flat storage */
list_t * process_queue; /* Ready queue */
list_t * sleep_queue;
volatile process_t * current_process = NULL;
process_t * kernel_idle_task = NULL;

static spin_lock_t tree_lock = { 0 };
static spin_lock_t process_queue_lock = { 0 };
static spin_lock_t wait_lock_tmp = { 0 };
static spin_lock_t sleep_lock = { 0 };

static bitset_t pid_set;

/* Default process name string */
char * default_name = "[unnamed]";

int is_valid_process(process_t * process) {
	foreach(lnode, process_list) {
		if (lnode->value == process) {
			return 1;
		}
	}

	return 0;
}

/*
 * This makes a nice 4096-byte bitmap. It also happens
 * to be pid_max on 32-bit Linux, so that's kinda nice.
 */
#define MAX_PID 32768

/*
 * Initialize the process tree and ready queue.
 */
void initialize_process_tree(void) {
	process_tree = tree_create();
	process_list = list_create();
	process_queue = list_create();
	sleep_queue = list_create();

	/* Start off with enough bits for 64 processes */
	bitset_init(&pid_set, MAX_PID / 8);
	/* First two bits are set by default */
	bitset_set(&pid_set, 0);
	bitset_set(&pid_set, 1);
}

/*
 * Recursively print a process node to the console.
 *
 * @param node   Node to print.
 * @param height Current depth in the tree.
 */
void debug_print_process_tree_node(tree_node_t * node, size_t height) {
	/* End recursion on a blank entry */
	if (!node) return;
	char * tmp = malloc(512);
	memset(tmp, 0, 512);
	char * c = tmp;
	/* Indent output */
	for (uint32_t i = 0; i < height; ++i) {
		c += sprintf(c, "  ");
	}
	/* Get the current process */
	process_t * proc = (process_t *)node->value;
	/* Print the process name */
	c += sprintf(c, "%d.%d %s", proc->group ? proc->group : proc->id, proc->id, proc->name);
	if (proc->description) {
		/* And, if it has one, its description */
		c += sprintf(c, " %s", proc->description);
	}
	if (proc->finished) {
		c += sprintf(c, " [zombie]");
	}
	/* Linefeed */
	debug_print(NOTICE, "%s", tmp);
	free(tmp);
	foreach(child, node->children) {
		/* Recursively print the children */
		debug_print_process_tree_node(child->value, height + 1);
	}
}

/*
 * Print the process tree to the console.
 */
void debug_print_process_tree(void) {
	debug_print_process_tree_node(process_tree->root, 0);
}

/*
 * Retreive the next ready process.
 * XXX: POPs from the ready queue!
 *
 * @return A pointer to the next process in the queue.
 */
process_t * next_ready_process(void) {
	if (!process_available()) {
		return kernel_idle_task;
	}
	if (process_queue->head->owner != process_queue) {
		debug_print(ERROR, "Erroneous process located in process queue: node 0x%x has owner 0x%x, but process_queue is 0x%x", process_queue->head, process_queue->head->owner, process_queue);

		process_t * proc = process_queue->head->value;

		debug_print(ERROR, "PID associated with this node is %d", proc->id);
	}
	node_t * np = list_dequeue(process_queue);
	assert(np && "Ready queue is empty.");
	process_t * next = np->value;
	return next;
}

/*
 * Reinsert a process into the ready queue.
 *
 * @param proc Process to reinsert
 */
void make_process_ready(process_t * proc) {
	if (proc->sleep_node.owner != NULL) {
		if (proc->sleep_node.owner == sleep_queue) {
			/* XXX can't wake from timed sleep */
			if (proc->timed_sleep_node) {
				IRQ_OFF;
				spin_lock(sleep_lock);
				list_delete(sleep_queue, proc->timed_sleep_node);
				spin_unlock(sleep_lock);
				IRQ_RES;
				proc->sleep_node.owner = NULL;
				free(proc->timed_sleep_node->value);
			}
			/* Else: I have no idea what happened. */
		} else {
			proc->sleep_interrupted = 1;
			spin_lock(wait_lock_tmp);
			list_delete((list_t*)proc->sleep_node.owner, &proc->sleep_node);
			spin_unlock(wait_lock_tmp);
		}
	}
	if (proc->sched_node.owner) {
		debug_print(WARNING, "Can't make process ready without removing from owner list: %d", proc->id);
		debug_print(WARNING, "  (This is a bug) Current owner list is 0x%x (ready queue is 0x%x)", proc->sched_node.owner, process_queue);
		return;
	}
	spin_lock(process_queue_lock);
	list_append(process_queue, &proc->sched_node);
	spin_unlock(process_queue_lock);
}


extern void tree_remove_reparent_root(tree_t * tree, tree_node_t * node);

/*
 * Delete a process from the process tree
 *
 * @param proc Process to find and remove.
 */
void delete_process(process_t * proc) {
	tree_node_t * entry = proc->tree_entry;

	/* The process must exist in the tree, or the client is at fault */
	if (!entry) return;

	/* We can not remove the root, which is an error anyway */
	assert((entry != process_tree->root) && "Attempted to kill init.");

	if (process_tree->root == entry) {
		/* We are init, don't even bother. */
		return;
	}

	/* Remove the entry. */
	spin_lock(tree_lock);
	/* Reparent everyone below me to init */
	int has_children = entry->children->length;
	tree_remove_reparent_root(process_tree, entry);
	list_delete(process_list, list_find(process_list, proc));
	spin_unlock(tree_lock);

	if (has_children) {
		process_t * init = process_tree->root->value;
		wakeup_queue(init->wait_queue);
	}

	bitset_clear(&pid_set, proc->id);

	/* Uh... */
	free(proc);
}

static void _kidle(void) {
	while (1) {
		IRQ_ON;
		PAUSE;
	}
}

/*
 * Spawn the idle "process".
 */
process_t * spawn_kidle(void) {
	process_t * idle = malloc(sizeof(process_t));
	memset(idle, 0x00, sizeof(process_t));
	idle->id = -1;
	idle->name = strdup("[kidle]");
	idle->is_tasklet = 1;

	idle->image.stack = (uintptr_t)malloc(KERNEL_STACK_SIZE) + KERNEL_STACK_SIZE;
	idle->thread.eip  = (uintptr_t)&_kidle;
	idle->thread.esp  = idle->image.stack;
	idle->thread.ebp  = idle->image.stack;

	idle->started = 1;
	idle->running = 1;
	idle->wait_queue = list_create();
	idle->shm_mappings = list_create();
	idle->signal_queue = list_create();

	gettimeofday(&idle->start, NULL);

	set_process_environment(idle, current_directory);
	return idle;
}

/*
 * Spawn the initial process.
 *
 * @return A pointer to the new initial process entry
 */
process_t * spawn_init(void) {
	/* We can only do this once. */
	assert((!process_tree->root) && "Tried to regenerate init!");

	/* Allocate space for a new process */
	process_t * init = malloc(sizeof(process_t));
	/* Set it as the root process */
	tree_set_root(process_tree, (void *)init);
	/* Set its tree entry pointer so we can keep track
	 * of the process' entry in the process tree. */
	init->tree_entry = process_tree->root;
	init->id      = 1;       /* Init is PID 1 */
	init->group   = 0; /* thread group id (real PID) */
	init->job     = 1; /* process group id (jobs) */
	init->session = 1; /* session leader id */
	init->name    = strdup("init");  /* Um, duh. */
	init->cmdline = NULL;
	init->user    = 0;       /* UID 0 */
	init->real_user = 0;
	init->mask    = 022;     /* umask */
	init->status  = 0;       /* Run status */
	init->fds = malloc(sizeof(fd_table_t));
	init->fds->refs = 1;
	init->fds->length   = 0;  /* Initialize the file descriptors */
	init->fds->capacity = 4;
	init->fds->entries  = malloc(sizeof(fs_node_t *) * init->fds->capacity);
	init->fds->modes    = malloc(sizeof(int) * init->fds->capacity);
	init->fds->offsets  = malloc(sizeof(uint64_t) * init->fds->capacity);

	/* Set the working directory */
	init->wd_node = clone_fs(fs_root);
	init->wd_name = strdup("/");

	/* Heap and stack pointers (and actuals) */
	init->image.entry       = 0;
	init->image.heap        = 0;
	init->image.heap_actual = 0;
	init->image.stack       = initial_esp + 1;
	init->image.user_stack  = 0;
	init->image.size        = 0;
	init->image.shm_heap    = SHM_START; /* Yeah, a bit of a hack. */

	spin_init(init->image.lock);

	/* Process is not finished */
	init->finished = 0;
	init->suspended = 0;
	init->started = 1;
	init->running = 1;
	init->wait_queue = list_create();
	init->shm_mappings = list_create();
	init->signal_queue = list_create();
	init->signal_kstack = NULL; /* None yet initialized */

	init->sched_node.prev = NULL;
	init->sched_node.next = NULL;
	init->sched_node.value = init;

	init->sleep_node.prev = NULL;
	init->sleep_node.next = NULL;
	init->sleep_node.value = init;

	init->timed_sleep_node = NULL;

	init->is_tasklet = 0;

	set_process_environment(init, current_directory);

	/* What the hey, let's also set the description on this one */
	init->description = strdup("[init]");
	list_insert(process_list, (void *)init);

	return init;
}

/*
 * Get the next available PID
 *
 * @return A usable PID for a new process.
 */
static int _next_pid = 2;
pid_t get_next_pid(void) {
	if (_next_pid > MAX_PID) {
		int index = bitset_ffub(&pid_set);
		/*
		 * Honestly, we don't have the memory to really risk reaching
		 * the point where we have MAX_PID processes running
		 * concurrently, so this assertion should be "safe enough".
		 */
		assert(index != -1);
		bitset_set(&pid_set, index);
		return index;
	}
	int pid = _next_pid;
	_next_pid++;
	assert(!bitset_test(&pid_set, pid) && "Next PID already allocated?");
	bitset_set(&pid_set, pid);
	return pid;
}

/*
 * Disown a process from its parent.
 */
void process_disown(process_t * proc) {
	assert(process_tree->root && "No init, has the process tree been initialized?");

	/* Find the process in the tree */
	tree_node_t * entry = proc->tree_entry;
	/* Break it of from its current parent */
	spin_lock(tree_lock);
	tree_break_off(process_tree, entry);
	/* And insert it back elsewhere */
	tree_node_insert_child_node(process_tree, process_tree->root, entry);
	spin_unlock(tree_lock);
}

/*
 * Spawn a new process.
 *
 * @param parent The parent process to spawn the new one off of.
 * @return A pointer to the new process.
 */
process_t * spawn_process(volatile process_t * parent, int reuse_fds) {
	assert(process_tree->root && "Attempted to spawn a process without init.");

	/* Allocate a new process */
	debug_print(INFO,"   process_t {");
	process_t * proc = malloc(sizeof(process_t));
	memset(proc, 0, sizeof(process_t));
	debug_print(INFO,"   }");
	proc->id = get_next_pid(); /* Set its PID */
	proc->group = proc->id;    /* Set the GID */
	proc->name = strdup(parent->name); /* Use the default name */
	proc->description = NULL;  /* No description */
	proc->cmdline = parent->cmdline;

	/* Copy permissions */
	proc->user  = parent->user;
	proc->real_user = parent->real_user;
	proc->mask = parent->mask;

	/* Until specified otherwise */
	proc->job = parent->job;
	proc->session = parent->session;

	/* Zero out the ESP/EBP/EIP */
	proc->thread.esp = 0;
	proc->thread.ebp = 0;
	proc->thread.eip = 0;
	proc->thread.fpu_enabled = 0;
	memcpy((void*)proc->thread.fp_regs, (void*)parent->thread.fp_regs, 512);

	/* Set the process image information from the parent */
	proc->image.entry       = parent->image.entry;
	proc->image.heap        = parent->image.heap;
	proc->image.heap_actual = parent->image.heap_actual;
	proc->image.size        = parent->image.size;
	debug_print(INFO,"    stack {");
	proc->image.stack       = (uintptr_t)kvmalloc(KERNEL_STACK_SIZE) + KERNEL_STACK_SIZE;
	debug_print(INFO,"    }");
	proc->image.user_stack  = parent->image.user_stack;
	proc->image.shm_heap    = SHM_START; /* Yeah, a bit of a hack. */

	spin_init(proc->image.lock);

	assert(proc->image.stack && "Failed to allocate kernel stack for new process.");

	/* Clone the file descriptors from the original process */
	if (reuse_fds) {
		proc->fds = parent->fds;
		proc->fds->refs++;
	} else {
		proc->fds = malloc(sizeof(fd_table_t));
		proc->fds->refs     = 1;
		proc->fds->length   = parent->fds->length;
		proc->fds->capacity = parent->fds->capacity;
		debug_print(INFO,"    fds / files {");
		proc->fds->entries  = malloc(sizeof(fs_node_t *) * proc->fds->capacity);
		proc->fds->modes    = malloc(sizeof(int) * proc->fds->capacity);
		proc->fds->offsets  = malloc(sizeof(uint64_t) * proc->fds->capacity);
		assert(proc->fds->entries && "Failed to allocate file descriptor table for new process.");
		debug_print(INFO,"    ---");
		for (uint32_t i = 0; i < parent->fds->length; ++i) {
			proc->fds->entries[i] = clone_fs(parent->fds->entries[i]);
			proc->fds->modes[i]   = parent->fds->modes[i];
			proc->fds->offsets[i] = parent->fds->offsets[i];
		}
		debug_print(INFO,"    }");
	}

	/* As well as the working directory */
	proc->wd_node = clone_fs(parent->wd_node);
	proc->wd_name = strdup(parent->wd_name);

	/* Zero out the process status */
	proc->status = 0;
	proc->finished = 0;
	proc->suspended = 0;
	proc->started = 0;
	proc->running = 0;
	memset(proc->signals.functions, 0x00, sizeof(uintptr_t) * NUMSIGNALS);
	proc->wait_queue = list_create();
	proc->shm_mappings = list_create();
	proc->signal_queue = list_create();
	proc->signal_kstack = NULL; /* None yet initialized */

	proc->sched_node.prev = NULL;
	proc->sched_node.next = NULL;
	proc->sched_node.value = proc;

	proc->sleep_node.prev = NULL;
	proc->sleep_node.next = NULL;
	proc->sleep_node.value = proc;

	proc->timed_sleep_node = NULL;

	proc->is_tasklet = 0;

	gettimeofday(&proc->start, NULL);

	/* Insert the process into the process tree as a child
	 * of the parent process. */
	tree_node_t * entry = tree_node_create(proc);
	assert(entry && "Failed to allocate a process tree node for new process.");
	proc->tree_entry = entry;
	spin_lock(tree_lock);
	tree_node_insert_child_node(process_tree, parent->tree_entry, entry);
	list_insert(process_list, (void *)proc);
	spin_unlock(tree_lock);

	/* Return the new process */
	return proc;
}

uint8_t process_compare(void * proc_v, void * pid_v) {
	pid_t pid = (*(pid_t *)pid_v);
	process_t * proc = (process_t *)proc_v;

	return (uint8_t)(proc->id == pid);
}

process_t * process_from_pid(pid_t pid) {
	if (pid < 0) return NULL;

	spin_lock(tree_lock);
	tree_node_t * entry = tree_find(process_tree,&pid,process_compare);
	spin_unlock(tree_lock);
	if (entry) {
		return (process_t *)entry->value;
	}
	return NULL;
}

process_t * process_get_parent(process_t * process) {
	process_t * result = NULL;
	spin_lock(tree_lock);

	tree_node_t * entry = process->tree_entry;

	if (entry->parent) {
		result = entry->parent->value;
	}

	spin_unlock(tree_lock);
	return result;
}

/*
 * Wait for children.
 *
 * @param process Process doing the waiting.
 * @param pid     PID to wait for
 * @param status  [out] Where to put the status conditions of the waited-for process
 * @param options Options (unused)
 * @return A pointer to the process that broke the wait
 */
process_t * process_wait(process_t * process, pid_t pid, int * status, int options) {
	/* `options` is ignored */
	if (pid == -1) {
		/* wait for any child process */
	} else if (pid < 0) {
		/* wait for any porcess whose ->group == processes[abs(pid)]->group */
	} else if (pid == 0) {
		/* wait for any process whose ->group == process->group */
	} else {
		/* wait for processes[pid] */
	}
	return NULL;
}

/*
 * Wake up a sleeping process
 *
 * @param process Process to wake up
 * @param caller  Who woke it up
 * @return Don't know yet, but I think it should return something.
 */
int process_wake(process_t * process, process_t * caller) {

	return 0;
}

/*
 * Set the directory for a process.
 *
 * @param proc      Process to set the directory for.
 * @param directory Directory to set.
 */
void set_process_environment(process_t * proc, page_directory_t * directory) {
	assert(proc);
	assert(directory);

	proc->thread.page_directory = directory;
}

/*
 * Are there any processes available in the queue?
 * (Queue not empty)
 *
 * @return 1 if there are processes available, 0 otherwise
 */
uint8_t process_available(void) {
	return (process_queue->head != NULL);
}

/*
 * Append a file descriptor to a process.
 *
 * @param proc Process to append to
 * @param node The VFS node
 * @return The actual fd, for use in userspace
 */
uint32_t process_append_fd(process_t * proc, fs_node_t * node) {
	/* Fill gaps */
	for (unsigned int i = 0; i < proc->fds->length; ++i) {
		if (!proc->fds->entries[i]) {
			proc->fds->entries[i] = node;
			/* modes, offsets must be set by caller */
			proc->fds->modes[i] = 0;
			proc->fds->offsets[i] = 0;
			return i;
		}
	}
	/* No gaps, expand */
	if (proc->fds->length == proc->fds->capacity) {
		proc->fds->capacity *= 2;
		proc->fds->entries = realloc(proc->fds->entries, sizeof(fs_node_t *) * proc->fds->capacity);
		proc->fds->modes   = realloc(proc->fds->modes,   sizeof(int) * proc->fds->capacity);
		proc->fds->offsets = realloc(proc->fds->offsets, sizeof(uint64_t) * proc->fds->capacity);
	}
	proc->fds->entries[proc->fds->length] = node;
	/* modes, offsets must be set by caller */
	proc->fds->modes[proc->fds->length] = 0;
	proc->fds->offsets[proc->fds->length] = 0;
	proc->fds->length++;
	return proc->fds->length-1;
}

/*
 * dup2() -> Move the file pointed to by `s(ou)rc(e)` into
 *           the slot pointed to be `dest(ination)`.
 *
 * @param proc  Process to do this for
 * @param src   Source file descriptor
 * @param dest  Destination file descriptor
 * @return The destination file descriptor, -1 on failure
 */
uint32_t process_move_fd(process_t * proc, int src, int dest) {
	if ((size_t)src >= proc->fds->length || (dest != -1 && (size_t)dest >= proc->fds->length)) {
		return -1;
	}
	if (dest == -1) {
		dest = process_append_fd(proc, NULL);
	}
	if (proc->fds->entries[dest] != proc->fds->entries[src]) {
		close_fs(proc->fds->entries[dest]);
		proc->fds->entries[dest] = proc->fds->entries[src];
		proc->fds->modes[dest] = proc->fds->modes[src];
		proc->fds->offsets[dest] = proc->fds->offsets[src];
		open_fs(proc->fds->entries[dest], 0);
	}
	return dest;
}

int wakeup_queue(list_t * queue) {
	int awoken_processes = 0;
	while (queue->length > 0) {
		spin_lock(wait_lock_tmp);
		node_t * node = list_pop(queue);
		spin_unlock(wait_lock_tmp);
		if (!((process_t *)node->value)->finished) {
			make_process_ready(node->value);
		}
		awoken_processes++;
	}
	return awoken_processes;
}

int wakeup_queue_interrupted(list_t * queue) {
	int awoken_processes = 0;
	while (queue->length > 0) {
		spin_lock(wait_lock_tmp);
		node_t * node = list_pop(queue);
		spin_unlock(wait_lock_tmp);
		if (!((process_t *)node->value)->finished) {
			process_t * proc = node->value;
			proc->sleep_interrupted = 1;
			make_process_ready(proc);
		}
		awoken_processes++;
	}
	return awoken_processes;
}


int sleep_on(list_t * queue) {
	if (current_process->sleep_node.owner) {
		/* uh, we can't sleep right now, we're marked as ready */
		switch_task(0);
		return 0;
	}
	current_process->sleep_interrupted = 0;
	spin_lock(wait_lock_tmp);
	list_append(queue, (node_t *)&current_process->sleep_node);
	spin_unlock(wait_lock_tmp);
	switch_task(0);
	return current_process->sleep_interrupted;
}

int process_is_ready(process_t * proc) {
	return (proc->sched_node.owner != NULL);
}


void wakeup_sleepers(unsigned long seconds, unsigned long subseconds) {
	IRQ_OFF;
	spin_lock(sleep_lock);
	if (sleep_queue->length) {
		sleeper_t * proc = ((sleeper_t *)sleep_queue->head->value);
		while (proc && (proc->end_tick < seconds || (proc->end_tick == seconds && proc->end_subtick <= subseconds))) {

			if (proc->is_fswait) {
				proc->is_fswait = -1;
				process_alert_node(proc->process,proc);
			} else {
				process_t * process = proc->process;
				process->sleep_node.owner = NULL;
				process->timed_sleep_node = NULL;
				if (!process_is_ready(process)) {
					make_process_ready(process);
				}
			}
			free(proc);
			free(list_dequeue(sleep_queue));
			if (sleep_queue->length) {
				proc = ((sleeper_t *)sleep_queue->head->value);
			} else {
				break;
			}
		}
	}
	spin_unlock(sleep_lock);
	IRQ_RES;
}

void sleep_until(process_t * process, unsigned long seconds, unsigned long subseconds) {
	if (current_process->sleep_node.owner) {
		/* Can't sleep, sleeping already */
		return;
	}
	process->sleep_node.owner = sleep_queue;

	IRQ_OFF;
	spin_lock(sleep_lock);
	node_t * before = NULL;
	foreach(node, sleep_queue) {
		sleeper_t * candidate = ((sleeper_t *)node->value);
		if (candidate->end_tick > seconds || (candidate->end_tick == seconds && candidate->end_subtick > subseconds)) {
			break;
		}
		before = node;
	}
	sleeper_t * proc = malloc(sizeof(sleeper_t));
	proc->process     = process;
	proc->end_tick    = seconds;
	proc->end_subtick = subseconds;
	proc->is_fswait = 0;
	process->timed_sleep_node = list_insert_after(sleep_queue, before, proc);
	spin_unlock(sleep_lock);
	IRQ_RES;
}

void cleanup_process(process_t * proc, int retval) {
	proc->status   = retval;
	proc->finished = 1;

	list_free(proc->wait_queue);
	free(proc->wait_queue);
	list_free(proc->signal_queue);
	free(proc->signal_queue);
	free(proc->wd_name);


	if (proc->node_waits) {
		list_free(proc->node_waits);
		free(proc->node_waits);
		proc->node_waits = NULL;
	}
	debug_print(INFO, "Releasing shared memory for %d", proc->id);
	shm_release_all(proc);
	free(proc->shm_mappings);
	debug_print(INFO, "Freeing more mems %d", proc->id);
	if (proc->signal_kstack) {
		free(proc->signal_kstack);
	}

	release_directory(proc->thread.page_directory);

	debug_print(INFO, "Dec'ing fds for %d", proc->id);
	proc->fds->refs--;
	if (proc->fds->refs == 0) {
		debug_print(INFO, "Reached 0, all dependencies are closed for %d's file descriptors and page directories", proc->id);
		debug_print(INFO, "Going to clear out the file descriptors %d", proc->id);
		for (uint32_t i = 0; i < proc->fds->length; ++i) {
			if (proc->fds->entries[i]) {
				close_fs(proc->fds->entries[i]);
				proc->fds->entries[i] = NULL;
			}
		}
		debug_print(INFO, "... and their storage %d", proc->id);
		free(proc->fds->entries);
		free(proc->fds->offsets);
		free(proc->fds->modes);
		free(proc->fds);
		debug_print(INFO, "... and the kernel stack (hope this ain't us) %d", proc->id);
		free((void *)(proc->image.stack - KERNEL_STACK_SIZE));
	}
}

void reap_process(process_t * proc) {
	debug_print(INFO, "Reaping process %d; mem before = %d", proc->id, memory_use());
	free(proc->name);
	debug_print(INFO, "Reaped  process %d; mem after = %d", proc->id, memory_use());

	delete_process(proc);
	debug_print_process_tree();
}

static int wait_candidate(process_t * parent, int pid, int options, process_t * proc) {
	if (!proc) return 0;

	if (options & WNOKERN) {
		/* Skip kernel processes */
		if (proc->is_tasklet) return 0;
	}

	if (pid < -1) {
		if (proc->job == -pid || proc->id == -pid) return 1;
	} else if (pid == 0) {
		/* Matches our group ID */
		if (proc->job == parent->id) return 1;
	} else if (pid > 0) {
		/* Specific pid */
		if (proc->id == pid) return 1;
	} else {
		return 1;
	}
	return 0;
}

int waitpid(int pid, int * status, int options) {
	process_t * proc = (process_t *)current_process;
	if (proc->group) {
		proc = process_from_pid(proc->group);
	}

	debug_print(INFO, "waitpid(%s%d, ..., %d) (from pid=%d.%d)", (pid >= 0) ? "" : "-", (pid >= 0) ? pid : -pid, options, current_process->id, current_process->group);

	do {
		process_t * candidate = NULL;
		int has_children = 0;

		/* First, find out if there is anyone to reap */
		foreach(node, proc->tree_entry->children) {
			if (!node->value) {
				continue;
			}
			process_t * child = ((tree_node_t *)node->value)->value;

			if (wait_candidate(proc, pid, options, child)) {
				has_children = 1;
				if (child->finished) {
					candidate = child;
					break;
				}
				if ((options & WSTOPPED) && child->suspended) {
					candidate = child;
					break;
				}
			}
		}

		if (!has_children) {
			/* No valid children matching this description */
			debug_print(INFO, "No children matching description.");
			return -ECHILD;
		}

		if (candidate) {
			debug_print(INFO, "Candidate found (%x:%d), bailing early.", candidate, candidate->id);
			if (status) {
				*status = candidate->status;
			}
			int pid = candidate->id;
			if (candidate->finished) {
				reap_process(candidate);
			}
			return pid;
		} else {
			if (options & WNOHANG) {
				return 0;
			}
			debug_print(INFO, "Sleeping until queue is done.");
			/* Wait */
			if (sleep_on(proc->wait_queue) != 0) {
				debug_print(INFO, "wait() was interrupted");
				return -EINTR;
			}
		}
	} while (1);
}

int process_wait_nodes(process_t * process,fs_node_t * nodes[], int timeout) {
	assert(!process->node_waits && "Tried to wait on nodes while already waiting on nodes.");

	fs_node_t ** n = nodes;
	int index = 0;
	if (*n) {
		do {
			int result = selectcheck_fs(*n);
			if (result < 0) {
				debug_print(NOTICE, "An invalid descriptor was specified: %d (0x%x) (pid=%d)", index, *n, current_process->id);
				return -1;
			}
			if (result == 0) {
				return index;
			}
			n++;
			index++;
		} while (*n);
	}

	if (timeout == 0) {
		return -2;
	}

	n = nodes;

	process->node_waits = list_create();
	if (*n) {
		do {
			if (selectwait_fs(*n, process) < 0) {
				debug_print(NOTICE, "Bad selectwait? 0x%x", *n);
			}
			n++;
		} while (*n);
	}

	if (timeout > 0) {
		debug_print(INFO, "fswait with a timeout of %d (pid=%d)", timeout, current_process->id);
		unsigned long s, ss;
		relative_time(0, timeout, &s, &ss);

		IRQ_OFF;
		spin_lock(sleep_lock);
		node_t * before = NULL;
		foreach(node, sleep_queue) {
			sleeper_t * candidate = ((sleeper_t *)node->value);
			if (candidate->end_tick > s || (candidate->end_tick == s && candidate->end_subtick > ss)) {
				break;
			}
			before = node;
		}
		sleeper_t * proc = malloc(sizeof(sleeper_t));
		proc->process     = process;
		proc->end_tick    = s;
		proc->end_subtick = ss;
		proc->is_fswait = 1;
		list_insert(((process_t *)process)->node_waits, proc);
		process->timeout_node = list_insert_after(sleep_queue, before, proc);
		spin_unlock(sleep_lock);
		IRQ_RES;
	} else {
		process->timeout_node = NULL;
	}

	process->awoken_index = -1;
	/* Wait. */
	switch_task(0);

	return process->awoken_index;
}

int process_awaken_from_fswait(process_t * process, int index) {
	process->awoken_index = index;
	list_free(process->node_waits);
	free(process->node_waits);
	process->node_waits = NULL;
	if (process->timeout_node && process->timeout_node->owner == sleep_queue) {
		sleeper_t * proc = process->timeout_node->value;
		if (proc->is_fswait != -1) {
			list_delete(sleep_queue, process->timeout_node);
			free(process->timeout_node->value);
			free(process->timeout_node);
		}
	}
	process->timeout_node = NULL;
	make_process_ready(process);
	return 0;
}

int process_alert_node(process_t * process, void * value) {

	if (!is_valid_process(process)) {
		debug_print(WARNING, "Invalid process in alert from fswait.");
		return 0;
	}



	if (!process->node_waits) {
		return 0; /* Possibly already returned. Wait for another call. */
	}

	int index = 0;
	foreach(node, process->node_waits) {
		if (value == node->value) {
			return process_awaken_from_fswait(process, index);
		}
		index++;
	}

	return -1;
}

