#include "cgi.h"
#include <stdio.h>

extern char *cgi_file_path;

int main() {
	int loop;
	CGI_FILE *received_file;

	cgi_file_path = "/tmp/received.";

	loop = cgi_init();
	if(loop) {
		if(loop == CGI_ERROR_LOCAL) printf("Local access denied.\n"), exit(1);
 
		printf("Content-type: text/plain\n\n");
		if(loop == CGI_ERROR_ENV) printf("Garbled environment.\n");
		else if(loop == CGI_ERROR_ALLOC) printf("Out of memory.\n");
		else if(loop == CGI_ERROR_READ) printf("I/O Error.  Shouldn't happen.\n");
		else printf("Shouldn't happen.\n");
		exit(0);
	}

//	cgi_dump_vars();

	printf("Content-type: text/html\n\n");

	printf("Some pretty headers...<BR>\n");
	printf("hidden1 = %s<BR>\n", cgi_param("hidden1"));

	printf("%i files sent.<BR>\n", cgi_numfiles("file1"));
	if((received_file = cgi_file("file1")))
		printf("file1:  Sent_Name = %s, Real_Name = %s, Content_Type = %s, size = %i, error = %i<BR>\n",
			received_file->Sent_Name, received_file->Real_Name, received_file->Content_Type, (int)received_file->size, received_file->error);

	printf("text1 = %s<BR>\n", cgi_param("text1"));
	printf("password1 = %s<BR>\n", cgi_param("password1"));
	printf("radio1 = %s<BR>\n", cgi_param("radio1"));

	printf("%i multiple1's.<BR>\n", cgi_numparams("multiple1"));
	printf("%i mult's.<BR>\n", cgi_numparams("mult"));
	cgi_flags |= CGI_PARTIAL_MATCH;
	printf("%i mult's (partial match).<BR>\n", cgi_numparams("mult"));
	for(loop = 1; cgi_paramn("multiple1", loop); loop++)
		printf("multiple1 = %s<BR>\n", cgi_paramn("multiple1", loop));

	printf("dropdown1 = %s<BR>\n", cgi_param("dropdown1"));

	return 0;
}
