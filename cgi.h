/*
    If you have a cgi that used the previous version, it should mostly work
    without modification if you add #define CGI_OLD_1.  I changed the interface
    because it was cluttering up the global namespace with unnecessarily common
    names.  Now all the global identifiers that this adds start with "cgi_" or
    "CGI_".

    Functions:
	The following access functions return NULL on not-found and call cgi_init() if needed.
	    cgi_param("name");		returns a ptr to the value
	    cgi_paramn("name", 5);	returns the 5th occurance of "name"
	    cgi_paramn("name", -5);	returns the 5th to the last occurance of "name"
	    cgi_cookie("name");		returns a ptr to the cookie
	    cgi_cookien("name", 5);	returns the 5th occurance of "name"
	    cgi_cookien("name", -5);	returns the 5th to the last occurance of "name"
	    cgi_file("name");		returns a ptr to a struct describing the file
	    cgi_filen("name", 5);	returns the 5th occurance of "name"
	    cgi_filen("name", -5);	returns the 5th to the last occurance of "name"
	    cgi_numparams("name");	returns the number of parameters matching "name"
	    cgi_numcookies("name");	returns the number of cookies matching "name"
	    cgi_numfiles("name");	returns the number of received files matching "name"

	Receiving files:
	    The struct returned by cgi_file() is defined as follows:

	    typedef struct {
		char *Name, *Sent_Name, *Real_Name, *Content_Type;
		off_t size;
		int error;
	    } CGI_FILE;

	    (Depend on element names, not locations, because the order may
	    change and new fields may be added in the future.)

	    Name is the name of the input field.  Send_Name is the filename of
	    the file on the sending computer (with the path stripped).
	    Real_Name is the full path to the file on the local machine.
	    Content_Type is whatever the browser says it is, size is the size
	    of the file in bytes.

	    If there was a write error, the file is removed and skipped,
	    Real_Name is set to NULL and error is set to the errno that
	    occurred.

	    For files to be stored, you must set cgi_file_path to point to the
	    name of a directory to store the files in.  (see below)

	    If error == 0 and Real_Name == NULL, then no file was sent.
	    Stupidly, browsers will send a zero-byte file even if no file was
	    selected to be uploaded or if the user specified a non-existant
	    file.  If error != 0 and Real_Name == NULL, a local write error
	    occurred.

	cgi_init()
	    parses the cgi's input but does nothing else.  It is called
	    automatically on the first access to the above functions.
	    Everything in Cgi_Storage is meaningless until after cgi_init() is
	    called.  cgi_init() may be manually called any time before the first
	    attempt to access a variable.  For command-line debugging, this is
	    where the user is prompted for the query string and cookies.

	    Returns:
		0 = success
		CGI_ERROR_LOCAL = locally run, but allow_local == 0
		CGI_ERROR_ENV = an env var was messed up
		CGI_ERROR_ALLOC = failed malloc(), alloca() or remalloc()
		CGI_ERROR_READ = Error in input data

	cgi_dump_vars()
	    displays all the variables passed and their values.  This
	    calls cgi_init() if needed.  Good for debugging, not recommended
	    for end-product cgi's.  This function may not exist in future
	    versions, or it may change entirely.

	cgi_clear_uploads()
	    unlinks all the uploaded files, useful at then end of a CGI, and may
	    be useful in conjunction with atexit().  This ignores any files that
	    already disappeared, but note that if you use this at all, no files
	    should be deleted before you call this, or it could accidentally
	    delete another instance's files.

	cgi_clear_env()
	    clears the environment variables left by CGI_KEEP_ENV

	cgi_chomp(string)
	    works like perl's chomp.  Hacks off \r\n, \n, \r, or \n\r from the
	    end of a string.

	cgi_fix_escape(char *string)
	    Fixes escaped stuff in CGI input strings.  *string is the string
	    to be fixed.  This happens AFTER parsing the string, and *not* more
	    than once.  It is already done for you.  It does nothing to a NULL
	    ptr.  This changes + to space and %nn to the correct character.

	cgi_strip(char *string, char *chars)
	    Strips out whatever is in chars from a string.  This is useful for
	    stripping "\r" from textareas sent to unix systems.

    Globals:
	Cgi_Storage stores all the actual data.  Members:
	    int Query_Num = number of query variables (from GET or POST)
		(the maximum value of n) + 1
	    char *Query_Data = stores data, contains a lot of \0's.
	    char *Query_Vars[n].Name = "name" in "name=value"
	    char *Query_Vars[n].Value = "value in "name=value"
	    int Query_Type = QUERY_TYPE_LOCAL or QUERY_TYPE_GET or
		QUERY_TYPE_POST or QUERY_TYPE_MULTI_PART

	    int Cookie_Num = number of cookies
		(the maximum value of n) + 1
	    char *Cookie_Data = stores data, contains a lot of \0's.
	    char *Cookie_Vars[n].Name = "name" in "name=value"
	    char *Cookie_Vars[n].Value = "value in "name=value"

	    int File_Num = number of files uploaded
	    CGI_FILE *Files = info about each file uploaded
	    char *Files[n].Name = variable name of the file
	    char *Files[n].Real_Name = local filename
	    char *Files[n].Sent_Name = filename sent by client
	    char *Files[n].Content_Type = Content-type, as sent by the client
	    off_t Files[n].size = size of the file, in bytes
	    int Files[n].error = the error that occured while creating the file
		0 == no error.  If Name != NULL, the file exists and had a write
		error after being opened.  The partial file was left alone and
		should be cleaned up.

	    int Secure == 1 if https is on (or local).

	    It is possable to get a NULL pointer in the Value if they passed
	    a variable with no equal sign like so: name=value&name&name=value.
	    There should never be a NULL ptr in the Name part.  name=&name=
	    will put null strings into value, not NULL ptrs.

	char *cgi_file_path	(default = NULL)
	    Specifies where to store files send by multi-part.  If this is not
	    set, sent files will be ignored.  Include the trailing '/' or what
	    to start the filename with.  This must be an absolute path.
	    This must be set before any cgi functions are called.

	off_t cgi_max_fsize	(default = 0)
	    Set the maximum size of a single file.  0 = no size limit.  If a
	    file exceeds max_fsize, the error for that file is set to EFBIG.
	    Only relavent if cgi_file_path is set.
	    This must be set before any cgi functions are called.

	int cgi_max_files	(default = 0)
	    Set the maximum number of files to be received.  0 = no limit.  If
	    more files are sent, they are ignored.
	    Only relavent if cgi_file_path is set.
	    This must be set before any cgi functions are called.

	unsigned int cgi_flags	(default = 0)
	    Set this to the bit-wise OR of the following:
	    CGI_KEEP_ENV:
		Setting this means cgi_init() will leave parameters and
		cookies in the environment.  Normally they are cleared for
		security, although they are still there briefly before the
		parsing is done.  Only send passwords via POST, never with
		GET or through cookies.
		This must be set before any cgi functions are called.
	    CGI_PREVENT_LOCAL:
		Prevent local test mode.  If this is set, cgi_init() will
		not attempt to get parameters from the terminal if this is
		called outside the web server.
		This must be set before any cgi functions are called.
	    CGI_IGNORE_CASE:
		Normally, case is matched exactly.  If this is set, the access
		functions will match names without regard to case.
		This flag may be modified at any time.  Subsequent calls
		to the relavent functions will honor the change.
	    CGI_PARTIAL_MATCH:
		If this is set, cgi_param("a") will match "a", "ab", "a2", etc.
		This affects all access functions.
		This flag may be modified at any time.  Subsequent calls
		to the relavent functions will honor the change.

    Notes:
	To compile a program that uses this, put #include <cgi.h> at the top
	and add "-lcgi" to the end of your cc or gcc line.

	Init sets the env vars QUERY_STRING and HTTP_COOKIE to "XXXXX" for
	security except if modify_env = 0.  However, POST is always the most
	secure way, because there still exists the race condition of getting the
	environment variable before it is overwritten.  Therefore when GETting,
	cgi_init() should be called as early as possable.  Beware that GET
	queries show all the parameters in most httpds' access logs.

	cgi_init() always reads the QUERY_STRING, even in POST or local mode.

	The only limit on sizes are the amount of memory the server can allocate
	except in local mode there is an 8k limit on the query and the cookie.
	This limit may disappear in the future, but local mode is only supposed
	to be for testing anyway.

	Names and Values are actually pointers to things inside Data.  It is
	NOT wise to modify the data pointed to by any of these functions.
	In particular, making the data longer will overwrite other variables or
	seg fault.

	Env vars used:  REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH,
	CONTENT_TYPE, HTTP_COOKIE, HTTPS, HTTP_USER_AGENT

	For compatibility, you should not start any function names, global
	variable names, or pre-processor defines with "cgi" or "CGI" or "Cgi".

	Any function may be a macro, or may change to a macro in the future.

    New Features:
	max memory size
	trap_segv() on a segmentation fault, displays strerror and dump_vars()
	clear_memory() function...maybe.
	set_cookie() function
	    Set-Cookie: NAME=VALUE; expires=Wdy, DD-Mon-YYYY HH:MM:SS GMT;
	    path=PATH; domain=DOMAIN_NAME; secure
	feature to store files in memory...maybe.

    Bugs:
	When compiling, "-lcgi" must come after modules that depend on it.  I
	    have no idea why, but if you know how to fix it, let me know.
	Never tested with C++
	multi-part problems
	    embedded multi-part isn't supported...it's never used anyway
	    content-type of non-files is not supported...it's always the same
		anyway and usually isn't sent except by lynx.
	    MIME and other encoded stuff isn't supported...it's never used
		anyway
	filename separators should be tested on Mac and Amiga, but they prob'ly
	    work.  At the worst, the original filename may be reported wrong,
	    but it will never have a / in it.
*/

#ifndef _CGI_H
#define _CGI_H

#include <sys/types.h>

#define cgi_param(name) cgi_paramn(name, 1)
#define cgi_cookie(name) cgi_cookien(name, 1)
#define cgi_file(name) cgi_filen(name, 1)

#define CGI_QUERY_LOCAL         1
#define CGI_QUERY_GET           2
#define CGI_QUERY_POST          3
#define CGI_QUERY_MULTI_PART    4

#define CGI_ERROR_LOCAL         1	/* if allow_local = 0 and !getenv("REQUEST_METHOD") */
#define CGI_ERROR_ENV           2	/* the calling server didn't set the env vars right */
#define CGI_ERROR_ALLOC         3	/* malloc, realloc, or alloca failed */
#define CGI_ERROR_READ          4	/* may happen on multi-part */

/* settings for cgi_flags (see above) */
#define CGI_KEEP_ENV            1
#define CGI_PREVENT_LOCAL       2
#define CGI_IGNORE_CASE         4
#define CGI_PARTIAL_MATCH       8


#ifdef __cplusplus
extern "C" {
#endif

struct Cgi_Var_Type {
    char *Name;
    char *Value;
};
typedef struct {
    char *Name;
    char *Sent_Name;
    char *Real_Name;
    char *Content_Type;
    off_t size;
    int error;
} CGI_FILE;
struct Cgi_Storage_t {
    int Query_Num;
    char *Query_Data;
    struct Cgi_Var_Type *Query_Vars;
    int Query_Type;

    int Cookie_Num;
    char *Cookie_Data;
    struct Cgi_Var_Type *Cookie_Vars;

    int File_Num;
    CGI_FILE *Files;

    int Secure;
};

extern struct Cgi_Storage_t Cgi_Storage;
extern char *cgi_file_path;
extern off_t cgi_max_fsize;
extern int cgi_max_files;
extern unsigned int cgi_flags;

void cgi_fix_escape(char *input);
void cgi_strip(char *string, const char *chars);
void cgi_chomp(char *string);
int cgi_init();
char *cgi_paramn(const char *name, int num);
char *cgi_cookien(const char *name, int num);
CGI_FILE *cgi_filen(const char *name, int num);
int cgi_numparams(const char *name);
int cgi_numcookies(const char *name);
int cgi_numfiles(const char *name);
void cgi_dump_vars();
void cgi_clear_uploads();
void cgi_clear_env();

#ifdef __cplusplus
}
#endif

#ifdef CGI_OLD_1
#	define QUERY_TYPE_LOCAL         CGI_QUERY_LOCAL
#	define QUERY_TYPE_GET           CGI_QUERY_GET
#	define QUERY_TYPE_POST          CGI_QUERY_POST
#	define QUERY_TYPE_MULTI_PART    CGI_QUERY_MULT_PART

#	define LOCAL_ERROR              CGI_ERROR_LOCAL
#	define ENV_ERROR                CGI_ERROR_ENV
#	define ALLOC_ERROR              CGI_ERROR_ALLOC
#	define READ_ERROR               CGI_ERROR_READ

#	define fix_escape(a)            cgi_fix_escape(a)
#	define strip(a, b)              cgi_strip((a), (b))
#	define chomp(a)                 cgi_chomp(a)
#	define init()                   cgi_init()
#	define paramn(a, b)             cgi_paramn((a), (b))
#	define param(a)                 cgi_param(a)
#	define cookien(a, b)            cgi_cookien((a), (b))
#	define cookie(a)                cgi_cookie(a)
#	define dump_vars()              cgi_dump_vars()
#	define clear_uploads()          cgi_uploads()
#	define clear_env()              cgi_clear_env()
#endif

#endif  /* #ifndef _CGI_H */
