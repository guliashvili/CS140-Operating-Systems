//
// Created by GIORGI GULIASHVILI on 9/23/16.
//

#include <string.h>
#include <stdlib.h>
#include "exit.h"
#include "../parser.h"

int MyExit(command_explained  * cex){
	
	char *s;
	s = next_parameter_value(cex);
	if(s == NULL){
		exit(EXIT_SUCCESS);
	}else{
		exit(atoi(s));
	}
	return 1;
}


