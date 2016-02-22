
/*# console.c
  #
  # brahimi.karim@hotmail.fr
*/

int  max_chars_per_line =1000;
int  max_chars_per_word =50;
int  nb_chars_line=0;
char tab_line_cmd[max_chars_per_line];
char tabs_words
char file_environement_commands_[]="/etc/environment.conf";
char tab_mots[

// verifier si touche enter presser
int is_touch_enter_pressed(int key){
	if (key=="enter"){ return 1; }
	if (key!="enter"){ return 0; }
}

//verifie si cmd dans repertoire
int verify_if_cmd_in_repertory(char cmd_name ,char repertory)

return 1;

return 0;

}

//verifie si cmd exist
int verify_cmd_exist(char cmd_name ){
	int cmd_exist1=verify_if_cmd_in_repertory(cmd_name,file_environement_commands_[]
	int cmd_exist1=verify_if_cmd_in_repertory(cmd_name,".");
}

//ajouter charactere taper au clavier au tableau de la ligne de commande
int add_char_to_line_tab_cmd(int key){
	nb_char_line++;
	if(nb_char_line<=max_chars_per_line){ 
		tab_line_cmd[nb_chars_line]=key; 
		return 1;
	}else{
		return 0;
	}
}

//verifier si espaces et mettre mots de la ligne dans tabs
int put_line_tab_in_tabs_words(char tab_line_cmd){
	int nb_mot=1,j=1;
	
	for(int i=1;i<=nb_chars_line;i++)
		if(tab_line_cmd[i]==' '){ 
			nb_mot++; 
		}else{
                        if(j<max_chars_per_word){
				tab_mots[nb_mot][j]=tab_line_cmd[i]
				j++;
			}
		}
}


int exec(char cmd){


}

//executer la commande ou erreur et ajout dans la ligne cmd
int exec_or_add_key_cmd_line(int key){
	int entrer_pressed=is_touch_enter_pressed(int key);
	// si touche entrer a ete presser verifier si premier mot contien une commande existante
	if (entrer_pressed==1) { 
                put_line_tab_in_tabs_words();
		int cmd_exist=verify_cmd_exist(tab_mots[1]);
		add_char_to_line_tab_cmd(key);
	if (entrer_pressed==0) { add_char_to_line_tab_cmd(key); }
	// Si commande existe lexecuter ou message
	if(cmd_existe==1){ exec(cmd); }
	if(cmd_existe==0){ printk("commande introuvable"); }

}
