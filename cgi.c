//#define DEBUG

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "cgi.h"

#define LOCAL_SIZE		8192
#define CHUNK_SIZE		1024

struct Cgi_Storage_t Cgi_Storage;
off_t cgi_max_fsize = 0;
int cgi_max_files = 0;
char *cgi_file_path = 0;
unsigned int cgi_flags = 0;

void cgi_fix_escape(register char *input) {
	/* Writing to the existing string works, because it is always copying from
	 * ahead of where we're writing. */
	register int readoffset = 0;
	char work[3];

	if(!input) return;

	while(input[readoffset] != 0) {
		switch(input[readoffset]) {
		  case '+':
			*input = ' ';
			input++;
		  break;

		  case '%':
			if(input[readoffset + 1] == '0' && (input[readoffset + 2] == 'd' || input[readoffset + 2] == 'D')) {
				readoffset += 3;
				break;
			}

			work[0] = input[++readoffset];	/* First hex digit */
			work[1] = input[++readoffset];	/* Second hex digit */
			work[2] = 0;
			*input = strtol(work, 0, 16);
			input++;
		  break;

		  default:
			*input = input[readoffset];
			input++;
		  break;
		}
	}
	*input = 0;
	return;
}

void cgi_strip(char *input, const char *strip) {
	register int readoffset = 0;

	if(!input || !strip) return;

	while(input[readoffset] != 0) {
		if(strchr(strip, input[readoffset])) readoffset++;
		else *input = input[readoffset];

		input++;
	}
	*input = 0;
	return;
}

void cgi_chomp(char *string) {
	int end;
	if(!string) return;
	end = strlen(string);

	     if(string[end-1] == '\n' && string[end-2] == '\r') end -= 2;
	else if(string[end-1] == '\r' && string[end-2] == '\n') end -= 2;
	else if(string[end-1] == '\n') end--;
	else if(string[end-1] == '\r') end--;

	string[end] = 0;
	return;
}

/* we'll use this in cgi_init() because fread() doesn't gaurantee that nbytes
 * will be read. */
int cgi_fullread(u_char *buf, int nbytes) {
	int pos = 0, chr;

	while((pos < nbytes) && (chr = getchar()) != EOF) buf[pos++] = chr;
	return pos;
}

/* Same as strstr(), but case insensitive. */
const char *cgi_strcasestr(const char *big, const char *little) {
	size_t len;

	if(!little) return big;
	len = strlen(little);
	while(*big) {
		if(!strncasecmp(big, little, len)) return big;
		big++;
	}
	return 0;
}

int cgi_init() {
    size_t data_size = 0;
    int loop;
    char *tmp;

    if(Cgi_Storage.Query_Data) return 0;  /* Don't initialize more than once. */
    if(cgi_file_path && *cgi_file_path != '/') cgi_file_path = 0;  /* must have absolute path. */

    /* Get the crap */
    if((tmp = getenv("QUERY_STRING")) && *tmp) {
	data_size = strlen(tmp) + 1;
	if(!(Cgi_Storage.Query_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
	memcpy(Cgi_Storage.Query_Data, tmp, data_size);
	if(!(cgi_flags&CGI_KEEP_ENV)) {
	    memset(tmp, 'X', data_size - 1);
	    setenv("QUERY_STRING", "XXXXX", 1);
	}
    }

    tmp = getenv("REQUEST_METHOD");
    if(!tmp) {
	Cgi_Storage.Query_Type = CGI_QUERY_LOCAL;
	if(cgi_flags&CGI_PREVENT_LOCAL) {
	    return CGI_ERROR_LOCAL;
	}
	if(data_size) {
	    data_size += LOCAL_SIZE;
	    if(!(tmp = (char *)realloc(Cgi_Storage.Query_Data, data_size))) return CGI_ERROR_ALLOC;
	    Cgi_Storage.Query_Data = tmp;
	    tmp += data_size - LOCAL_SIZE;
	    tmp[-1] = '&';
	} else {
	    data_size = LOCAL_SIZE;
	    if(!(Cgi_Storage.Query_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
	    tmp = Cgi_Storage.Query_Data;
	}
	/* get local input - fgets() is okay because we're reading from a terminal */
	printf("Offline mode, enter query string (name=value; name=value, %i max):\n", LOCAL_SIZE);
	*tmp = 0;
	fgets(tmp, LOCAL_SIZE, stdin);
	cgi_chomp(tmp);
    } else if(!strcasecmp("POST", tmp)) {
	if(!(tmp = getenv("CONTENT_TYPE"))) return CGI_ERROR_ENV;
	if(!strcmp(tmp, "application/x-www-form-urlencoded")) {
	    int post_size;

	    Cgi_Storage.Query_Type = CGI_QUERY_POST;
	    if(!(tmp = getenv("CONTENT_LENGTH"))) return CGI_ERROR_ENV;
	    post_size = strtol(tmp, 0, 10)+1;        /* Leave room for \0 */
	    if(post_size < 1) return CGI_ERROR_ENV;  /* Garbage CONTENT_LENGTH */

	    if(post_size > 1) {		/* skip an empty post */
		if(data_size) {
		    data_size += post_size;
		    if(!(tmp = (char *)realloc(Cgi_Storage.Query_Data, data_size))) return CGI_ERROR_ALLOC;
		    Cgi_Storage.Query_Data = tmp;
		    tmp += data_size - post_size;
		    tmp[-1] = '&';
		} else {
		    data_size = post_size;
		    if(!(Cgi_Storage.Query_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
		    tmp = Cgi_Storage.Query_Data;
		}
		/* post input starts at tmp */
		if(cgi_fullread(tmp, post_size-1) != post_size-1) return CGI_ERROR_ENV; 
		tmp[post_size-1] = 0;
	    }
	} else if(!strncasecmp(tmp, "multipart/form-data; boundary=", 30)) {
	    u_char *input;
	    char *boundry, varname[CHUNK_SIZE], content_type[CHUNK_SIZE];
	    size_t total_query_size;
	    ssize_t boundry_len, read_size;
	    int first_query_var = 1; /* determines whether to append first */
	    int skip_file = 0, tmpint;
	    FILE *fp = 0;

	    Cgi_Storage.Query_Type = CGI_QUERY_MULTI_PART;

	    /* boundry = "\r\n--boundrycrap".  boundry_len is its length. */
	    boundry_len = strlen(tmp + 30) + 4;
	    if(!(boundry = (char *)alloca(boundry_len + 1))) return CGI_ERROR_ALLOC;
	    boundry[0] = '\r'; boundry[1] = '\n';
	    boundry[2] = boundry[3] = '-';
	    strcpy(boundry + 4, tmp + 30);

	    if(!(input = (u_char *)alloca(CHUNK_SIZE + boundry_len))) return CGI_ERROR_ALLOC;
	    /* prepare Cgi_Storage.Query_Data to have data appended */
	    if(data_size) {
		total_query_size = data_size - 1;
		data_size += CHUNK_SIZE;
		if(!(tmp = (char *)realloc(Cgi_Storage.Query_Data, data_size))) return CGI_ERROR_ALLOC;
		Cgi_Storage.Query_Data = tmp;
		first_query_var = 0;
	    } else {
		total_query_size = 0;
		data_size = CHUNK_SIZE;
		if(!(Cgi_Storage.Query_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
	    }

	    /*
	     * Post input starts at Cgi_Storage[total_query_size].  data_size
	     * is how much memory is allocated.
	     */

	    /* Reads initial boundry */
	    input[0] = '\r';
	    input[1] = '\n';
	    if(cgi_fullread(input+2, boundry_len-2) < boundry_len-2) return CGI_ERROR_READ;
	    if(strncmp(boundry, (char *)input, boundry_len)) return CGI_ERROR_READ;

	    while(1) {
		read_size = cgi_fullread(input + boundry_len, CHUNK_SIZE);
		if(read_size < 0) return CGI_ERROR_READ;
		if(read_size == 0) break;

		/*
		 * Basically, we're looking at CHUNK_SIZE and we have an
		 * additional boundry_len bytes read ahead that we leave for
		 * the next iteration.  Read_size is how much relavent data is
		 * in input (not including the extra boundry_len bytes).
		 * Since the last boundry_len bytes will be a boundry and --,
		 * we won't miss any data.
		 */
		for(loop = 0; loop < read_size; loop++) {
		    if(!strncmp((char *)input + loop, boundry, boundry_len)) {
			/* we found a boundry. */
			if(fp) {
			    if(fclose(fp) == EOF) {
				/* Write error */
				skip_file = 1;
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].error = errno;
				unlink(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				free(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name = 0;
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].size = 0;
			    } else if(Cgi_Storage.Files[Cgi_Storage.File_Num-1].size == 0) {
				/* zero-byte file usually means the client
				 * couldn't find the file or no file was
				 * selected to be sent. */
				unlink(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				free(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name = 0;
			    }
			    fp = 0;
			}
			skip_file = 0;

			/*
			 * Put the headers at the beginning of input.
			 * loop = how much to skip.
			 */
			memmove(input, input + loop, read_size + boundry_len - loop);
			read_size -= loop;
			/*
			 * skip the boundry
			 */
			loop = boundry_len;
			/*
			 * input[loop] is now just after the boundry.
			 * input[read_size + boundry_len] is the end of the input
			 *
			 * fill in more input.  Notice that CHUNK_SIZE must be
			 * greater than the maximum length of a header.
			 */
			if((tmpint = cgi_fullread(input + read_size + boundry_len, CHUNK_SIZE - read_size)) > 0) {
			    read_size += tmpint;
			}

#ifdef DEBUG
printf("tmpint (amt read): %i data_size: %i boundry_len: %i read_size: %i total_query_size: %i\n", tmpint, data_size, boundry_len, read_size, total_query_size);
#endif

			/* now the headers begin at *input */
			if(input[loop] == '-' && input[loop+1] == '-') {
			    /* WE'RE DONE HERE */
			    break;
			}
			if(input[loop] != '\r' || input[loop+1] != '\n') return CGI_ERROR_READ;
			loop += 2;
			/*
			 * leaves input + loop at the beginning of the headers.
			 */

			*varname = 0;
			*content_type = 0;
			while(1) {    /* header loop */
			    int inquotes;
			    inquotes = 0;

			    /* skip spaces and ;'s to find next header variable */
			    while(strchr("; ", input[loop])) loop++;

			    /* Is this the end of the headers? */
			    if(!strncmp((char *)input + loop, "\r\n\r\n", 4)) {
				loop += 3;  /* outer while loop ++'s once more */
				break;
			    }

			    /* skip any single line breaks too... */
			    while(strchr("\r\n", input[loop])) loop++;

			    if(!strncasecmp((char *)input + loop, "name=", 5)) {
				loop += 5;
				tmpint = 0;
				while(inquotes || (input[loop] != ';' && input[loop] != '\r')) {
				    if(input[loop] == '"') {
					inquotes = !inquotes;
				    } else if(input[loop] == '%') {
					varname[tmpint++] = '%';
					varname[tmpint++] = '2';
					varname[tmpint++] = '5';
				    } else if(input[loop] == '&') {
					varname[tmpint++] = '%';
					varname[tmpint++] = '2';
					varname[tmpint++] = '6';
				    } else if(input[loop] == '=') {
					varname[tmpint++] = '%';
					varname[tmpint++] = '3';
					varname[tmpint++] = 'd';
				    } else {
					varname[tmpint++] = (char)input[loop];
				    }
				    loop++;
				    if((loop >= read_size) || (tmpint >= CHUNK_SIZE)) return CGI_ERROR_READ;
				}
				varname[tmpint++] = 0;
			    } else if(!strncasecmp((char *)input + loop, "filename=", 9)) {
				if(cgi_file_path) {
				    void *ptr;
				    char dir_separator = '/';

				    loop += 9;
				    if(cgi_max_files && Cgi_Storage.File_Num >= cgi_max_files) {
					skip_file = 1;
					break;   /* header loop */
				    }
				    Cgi_Storage.File_Num++;
				    if(!(ptr = realloc(Cgi_Storage.Files, Cgi_Storage.File_Num*sizeof(CGI_FILE)))) return CGI_ERROR_ALLOC;
				    Cgi_Storage.Files = (CGI_FILE *)ptr;
				    Cgi_Storage.Files[Cgi_Storage.File_Num-1].Name = 0;
				    Cgi_Storage.Files[Cgi_Storage.File_Num-1].Content_Type = 0;
				    Cgi_Storage.Files[Cgi_Storage.File_Num-1].size = 0;
				    Cgi_Storage.Files[Cgi_Storage.File_Num-1].error = 0;

				    /* Store Real_Name */
				    tmpint = strlen(cgi_file_path);
				    if(!(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name = (char *)malloc(tmpint+65))) return CGI_ERROR_ALLOC;
				    strcpy(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name, cgi_file_path);
				    strcpy(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name + tmpint, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");

				    /* Create file */
				    if(((tmpint = mkstemp(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name)) == -1)
					    || (fp = fopen(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name, "r+")) == NULL) {
					if(tmpint != -1) {
					    unlink(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
					    close(tmpint);
					}
					free(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
					Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name = 0;

					/* We store the error for the programmer to see */
					Cgi_Storage.Files[Cgi_Storage.File_Num-1].error = errno;
					skip_file = 1;
				    }

				    /* Calculate directory separator */
				    tmp = getenv("HTTP_USER_AGENT");
				    if(!tmp) return CGI_ERROR_ENV;

				    /* Windows machines separate at a \ */
				    if(cgi_strcasestr(tmp, "win")) dir_separator = '\\';
				    /* Mac machines separate at a : */
				    if(cgi_strcasestr(tmp, "macintosh")) dir_separator = ':';
				    /* Amiga should be tested for here... */

				    /* separate on / for everyone else.  This
				     * covers old Unix Netscapes and the
				     * potential security problem of a client
				     * sending "../../..". */

				    /* Store Sent_Name */
				    if(!(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Sent_Name = (char *)malloc(MAXPATHLEN))) return CGI_ERROR_ALLOC;
				    tmpint = 0;    /* length of the filename */
				    while(inquotes || (input[loop] != ';' && input[loop] != '\r')) {
					if(input[loop] == '"') inquotes = !inquotes;
					else if(input[loop] == '/' || input[loop] == dir_separator) tmpint = 0;
					else if(tmpint < MAXPATHLEN-1) Cgi_Storage.Files[Cgi_Storage.File_Num-1].Sent_Name[tmpint++] = (char)input[loop];
					loop++;
					if(loop >= read_size) return CGI_ERROR_READ;
				    }
				    Cgi_Storage.Files[Cgi_Storage.File_Num-1].Sent_Name[tmpint++] = 0;
				} else {
				    skip_file = 1;
				    /* skips filename field */
				    if(input[loop] == '"') inquotes = !inquotes;
				    loop++;
				    if(loop >= read_size) return CGI_ERROR_READ;
				}
			    } else if(!strncasecmp((char *)input + loop, "content-type: ", 14)) {
				loop += 14;
				tmpint = 0;
				while(inquotes || (input[loop] != ';' && input[loop] != '\r')) {
				    if(input[loop] == '"') inquotes = !inquotes;
				    else content_type[tmpint++] = (char)input[loop];
				    loop++;
				    if(loop >= read_size) return CGI_ERROR_READ;
				}
				content_type[tmpint++] = 0;
				/* it's safe from overflow, because read_size < sizeof(varname) */
			    } else while(inquotes || (input[loop] != ';' && input[loop] != '\r')) {
				/* skips unknown fields */
				if(input[loop] == '"') inquotes = !inquotes;
				loop++;
				if(loop >= read_size) return CGI_ERROR_READ;
			    }
			}    /* header loop */
			if(skip_file) {
			} else if(fp) {
			    /* Store Name */
			    cgi_fix_escape(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Name);
			    if(!(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Name = (char *)malloc(strlen(varname)+1))) return CGI_ERROR_ALLOC;
			    strcpy(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Name, varname);
			    /* Store Content_Type */
			    if(!(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Content_Type = (char *)malloc(strlen(content_type)+1))) return CGI_ERROR_ALLOC;
			    strcpy(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Content_Type, content_type);
			} else {
			    if(!first_query_var) Cgi_Storage.Query_Data[total_query_size++] = '&';
			    first_query_var = 0;

			    strcpy(Cgi_Storage.Query_Data + total_query_size, varname);
			    total_query_size += strlen(varname);
			    Cgi_Storage.Query_Data[total_query_size++] = '=';
			}
		    } else {
			/* no boundry here */
			if(skip_file) {
			} else if(fp) {
#ifdef DEBUG
static int asdf = 0;
asdf++;
fputc(input[loop], stdout);
if(input[loop] == '\n') printf(" %i ", asdf);
#endif
			    if((++Cgi_Storage.Files[Cgi_Storage.File_Num-1].size > cgi_max_fsize && cgi_max_fsize)
					|| fputc(input[loop], fp) == EOF) {
				/* Write error */
				fclose(fp);
				fp = 0;
				skip_file = 1;
				if(Cgi_Storage.Files[Cgi_Storage.File_Num-1].size > cgi_max_fsize) errno = EFBIG;
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].error = errno;
				unlink(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				free(Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name);
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].Real_Name = 0;
				Cgi_Storage.Files[Cgi_Storage.File_Num-1].size = 0;
			    }
			} else {
			    switch(input[loop]) {
				case 0:
				    break;
				case '%':
				    Cgi_Storage.Query_Data[total_query_size++] = '%';
				    Cgi_Storage.Query_Data[total_query_size++] = '2';
				    Cgi_Storage.Query_Data[total_query_size++] = '5';
				    break;
				case '&':
				    Cgi_Storage.Query_Data[total_query_size++] = '%';
				    Cgi_Storage.Query_Data[total_query_size++] = '2';
				    Cgi_Storage.Query_Data[total_query_size++] = '6';
				    break;
				case '=':
				    Cgi_Storage.Query_Data[total_query_size++] = '%';
				    Cgi_Storage.Query_Data[total_query_size++] = '3';
				    Cgi_Storage.Query_Data[total_query_size++] = 'd';
				    break;
				default:
				    Cgi_Storage.Query_Data[total_query_size++] = (char)input[loop];
			    }
			}

			/*
			 * At most, our antics add CHUNK_SIZE or 3 bytes per
			 * iteration, so we keep an extra CHUNK_SIZE allocated.
			 * When data_size < total_query_size + CHUNK_SIZE, we
			 * need more memory.
			 */
			if(data_size < total_query_size + CHUNK_SIZE) {
			    data_size += CHUNK_SIZE;
			    if(!(tmp = (char *)realloc(Cgi_Storage.Query_Data, data_size))) return CGI_ERROR_ALLOC;
			    Cgi_Storage.Query_Data = tmp;
			}
		    } /* header processor loop */
		} /* for loop for a single chunk */
		memmove(input, input + read_size, boundry_len);
	    } /* Chunk-reader loop */
	    Cgi_Storage.Query_Data[total_query_size++] = 0;
	} else return CGI_ERROR_ENV;	/* Garbage CONTENT_TYPE */
    } else if(!strcasecmp("GET", tmp)) {
	Cgi_Storage.Query_Type = CGI_QUERY_GET;
    } else return CGI_ERROR_ENV;	/* Garbage REQUEST_METHOD */

    if(!Cgi_Storage.Query_Data) {
	if(!(Cgi_Storage.Query_Data = (char *)malloc(1))) return CGI_ERROR_ALLOC;
	*Cgi_Storage.Query_Data = 0;
    }



    /* Allocate the Query_Vars pointers */
    data_size = 1;
    for(tmp = Cgi_Storage.Query_Data; *tmp; ++tmp)
	if(*tmp == '&') data_size++;
    if(!(Cgi_Storage.Query_Vars = (struct Cgi_Var_Type *)malloc(data_size*sizeof(struct Cgi_Var_Type)))) return CGI_ERROR_ALLOC;

    /* Parse the crap */
    Cgi_Storage.Query_Num = 0;
    tmp = Cgi_Storage.Query_Data;
    while(tmp && *tmp)
	Cgi_Storage.Query_Vars[Cgi_Storage.Query_Num++].Value = strsep(&tmp, "&");

    /* Do this later so that if &name& or && gets in the string it doesn't mess up. */
    for(loop = 0; loop < Cgi_Storage.Query_Num; loop++) {
	Cgi_Storage.Query_Vars[loop].Name = strsep(&Cgi_Storage.Query_Vars[loop].Value, "=");
	cgi_fix_escape(Cgi_Storage.Query_Vars[loop].Name);
	cgi_fix_escape(Cgi_Storage.Query_Vars[loop].Value);
    }



    /* Get the cookies */
    if((tmp = getenv("HTTP_COOKIE"))) {
	data_size = strlen(tmp) + 1;
	if(!(Cgi_Storage.Cookie_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
	memcpy(Cgi_Storage.Cookie_Data, tmp, data_size);
	if(!(cgi_flags&CGI_KEEP_ENV)) {
	    memset(tmp, 'X', data_size - 1);
	    setenv("HTTP_COOKIE", "XXXXX", 1);
	}
    } else if(Cgi_Storage.Query_Type == CGI_QUERY_LOCAL) {
	data_size = CHUNK_SIZE;
	if(!(Cgi_Storage.Cookie_Data = (char *)malloc(data_size))) return CGI_ERROR_ALLOC;
	printf("Offline mode, enter cookie string (name=value; name=value, %i max):\n", CHUNK_SIZE);
	*Cgi_Storage.Cookie_Data = 0;
	fgets(Cgi_Storage.Cookie_Data, data_size, stdin);
	cgi_chomp(Cgi_Storage.Cookie_Data);
    } else {
	if(!(Cgi_Storage.Cookie_Data = (char *)malloc(1))) return CGI_ERROR_ALLOC;
	*Cgi_Storage.Cookie_Data = 0;
    }

    /* Allocate the Cookie_Vars pointers */
    data_size = 1;
    for(tmp = Cgi_Storage.Cookie_Data; *tmp; tmp++)
	if(*tmp == ';') data_size++;
    if(!(Cgi_Storage.Cookie_Vars = (struct Cgi_Var_Type *)malloc(data_size*sizeof(struct Cgi_Var_Type)))) return CGI_ERROR_ALLOC;

    /* Parse the cookies (chocolate chip over here...M&M over there...mmm...) */
    Cgi_Storage.Cookie_Num = 0;
    tmp = Cgi_Storage.Cookie_Data;
    while(tmp && *tmp) {
	Cgi_Storage.Cookie_Vars[Cgi_Storage.Cookie_Num++].Value = strsep(&tmp, ";");
	if(tmp && *tmp == ' ') tmp++;	/* Skips the space after the ; */
    }

    /* Do this later so that if ;name; or ;; gets in the string it doesn't mess up. */
    for(loop = 0; loop < Cgi_Storage.Cookie_Num; loop++) {
	Cgi_Storage.Cookie_Vars[loop].Name = strsep(&Cgi_Storage.Cookie_Vars[loop].Value, "=");
	cgi_fix_escape(Cgi_Storage.Cookie_Vars[loop].Name);
	cgi_fix_escape(Cgi_Storage.Cookie_Vars[loop].Value);
    }



    /* Check for a secure connection */
    if((tmp = getenv("HTTPS")) && !strcasecmp("on", tmp)) Cgi_Storage.Secure = 1;
    if(Cgi_Storage.Query_Type == CGI_QUERY_LOCAL) Cgi_Storage.Secure = 1;

    return 0;
}

int cgi_strcmp(const char *str1, const char *str2, int len) {
	switch(cgi_flags&(CGI_IGNORE_CASE|CGI_PARTIAL_MATCH)) {
	  case 0:
		return strcmp(str1, str2);
	  case CGI_IGNORE_CASE:
		return strcasecmp(str1, str2);
	  case CGI_PARTIAL_MATCH:
		return strncmp(str1, str2, len);
	  case CGI_IGNORE_CASE|CGI_PARTIAL_MATCH:
		return strncasecmp(str1, str2, len);
	  default:
		return 1; /* never happens */
	}
}

char *cgi_paramn(const char *name, int num) {
	int loop, len;

	if(cgi_init()) return 0;
	if(!name) return 0;
	if(!num) return 0;
	len = strlen(name);

	if(num > 0) {
		for(loop = 0; loop < Cgi_Storage.Query_Num; loop++) {
			if(Cgi_Storage.Query_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Query_Vars[loop].Name, name, len)) {
				if(!--num) return Cgi_Storage.Query_Vars[loop].Value;
			}
		}
	} else {
		for(loop = Cgi_Storage.Query_Num - 1; loop >= 0; loop--) {
			if(Cgi_Storage.Query_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Query_Vars[loop].Name, name, len)) {
				if(!++num) return Cgi_Storage.Query_Vars[loop].Value;
			}
		}
	}
	return 0;        /* Not found */
}


char *cgi_cookien(const char *name, int num) {
	int loop, len;

	if(cgi_init()) return 0;
	if(!name) return 0;
	if(!num) return 0;
	len = strlen(name);

	if(num > 0) {
		for(loop = 0; loop < Cgi_Storage.Cookie_Num; loop++) {
			if(Cgi_Storage.Cookie_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Cookie_Vars[loop].Name, name, len)) {
				if(--num == 0) return Cgi_Storage.Cookie_Vars[loop].Value;
			}
		}
	} else {
		for(loop = Cgi_Storage.Cookie_Num - 1; loop >= 0; loop--) {
			if(Cgi_Storage.Cookie_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Cookie_Vars[loop].Name, name, len)) {
				if(++num == 0) return Cgi_Storage.Cookie_Vars[loop].Value;
			}
		}
	}
	return 0;	/* Not found */
}

CGI_FILE *cgi_filen(const char *name, int num) {
	int loop, len;

	if(cgi_init()) return 0;
	if(!name) return 0;
	if(!num) return 0;
	len = strlen(name);

	if(num > 0) {
		for(loop = 0; loop < Cgi_Storage.File_Num; loop++) {
			if(Cgi_Storage.Files[loop].Name && !cgi_strcmp(Cgi_Storage.Files[loop].Name, name, len)) {
				if(--num == 0) return &Cgi_Storage.Files[loop];
			}
		}
	} else {
		for(loop = Cgi_Storage.File_Num - 1; loop >= 0; loop--) {
			if(Cgi_Storage.Files[loop].Name && !cgi_strcmp(Cgi_Storage.Files[loop].Name, name, len)) {
				if(++num == 0) return &Cgi_Storage.Files[loop];
			}
		}
	}
	return 0;	/* Not found */
}

int cgi_numparams(const char *name) {
	int loop = 0, ret = 0, len;
	if(!name) return Cgi_Storage.Query_Num;
	len = strlen(name);
	while(loop < Cgi_Storage.Query_Num) {
		if(Cgi_Storage.Query_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Query_Vars[loop].Name, name, len))
			ret++;
		loop++;
	}
	return ret;
}

int cgi_numcookies(const char *name) {
	int loop = 0, ret = 0, len;
	if(!name) return Cgi_Storage.Cookie_Num;
	len = strlen(name);
	while(loop < Cgi_Storage.Cookie_Num) {
		if(Cgi_Storage.Cookie_Vars[loop].Name && !cgi_strcmp(Cgi_Storage.Cookie_Vars[loop].Name, name, len))
			ret++;
		loop++;
	}
	return ret;
}

int cgi_numfiles(const char *name) {
	int loop = 0, ret = 0, len;
	if(!name) return Cgi_Storage.File_Num;
	len = strlen(name);
	while(loop < Cgi_Storage.File_Num) {
		if(Cgi_Storage.Files[loop].Name && !cgi_strcmp(Cgi_Storage.Files[loop].Name, name, len))
			ret++;
		loop++;
	}
	return ret;
}

void cgi_dump_vars() {
	int loop;

	fflush(stdout);
	printf("Content-type: text/html\n\n<pre>\n");

	if((loop = cgi_init())) {
		printf("Error %i occured in cgi_init().\n\n", loop);
	}

	printf("Query Type:  ");
	switch (Cgi_Storage.Query_Type) {
	  case CGI_QUERY_LOCAL:
		printf("Local\n\n");
		break;
	  case CGI_QUERY_GET:
		printf("Get\n\n");
		break;
	  case CGI_QUERY_POST:
		printf("Post\n\n");
		break;
	  case CGI_QUERY_MULTI_PART:
		printf("Multi-part\n\n");
		break;
	  default:
		printf("Unknown\n\n");
	}

	printf("Query Variables:\n");
	for(loop = 0; loop < Cgi_Storage.Query_Num; loop++)
		printf("%s=%s.\n", Cgi_Storage.Query_Vars[loop].Name, Cgi_Storage.Query_Vars[loop].Value);

	printf("\nCookie Variables:\n");
	for(loop = 0; loop < Cgi_Storage.Cookie_Num; loop++)
		printf("%s=%s.\n", Cgi_Storage.Cookie_Vars[loop].Name, Cgi_Storage.Cookie_Vars[loop].Value);

	printf("\nUploaded files:\n");
	for(loop = 0; loop < Cgi_Storage.File_Num; loop++) {
		printf("File %s:\n", Cgi_Storage.Files[loop].Name);
		printf("\tReal_Name: %s\n", Cgi_Storage.Files[loop].Real_Name);
		printf("\tSent_Name: %s\n", Cgi_Storage.Files[loop].Sent_Name);
		printf("\tContent_Type: %s\n", Cgi_Storage.Files[loop].Content_Type);
		printf("\tsize: %qi\n", (long long)Cgi_Storage.Files[loop].size);
		printf("\terror: %i", Cgi_Storage.Files[loop].error);
		if(Cgi_Storage.Files[loop].error)
			printf(" - %s", strerror(Cgi_Storage.Files[loop].error));
		printf("\n");
	}
	printf("\nEnd of dump.\n</pre>\n");
	fflush(stdout);
	return;
}

void cgi_clear_uploads() {
	int loop = 0;
	while(loop < Cgi_Storage.File_Num)
		unlink(Cgi_Storage.Files[loop++].Real_Name);
}

void cgi_clear_env() {
	setenv("QUERY_STRING", "XXXXX", 1);
	setenv("HTTP_COOKIE", "XXXXX", 1);
}
