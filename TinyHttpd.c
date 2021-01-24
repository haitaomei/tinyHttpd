#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>


#define ISspace(x)		isspace((int)(x))

#define SERVER_STRING	"Server: TinyHttpd/0.1`/0\r\n"
#define STDIN	0
#define STDOUT	1
#define STDERR	2

void accept_request(void*);
void cat(int, FILE*);
void error_die(const char*);
int get_line(int, char*, int);
void headers(int, const char*);
void not_found(int);
void serve_file(int, const char*);
int startup(u_short*);
void unimplemented(int);


int main() {
	int server_sock = -1;
	u_short port = 1989;
	int client_sock = -1;
	struct sockaddr_in client_name;
	socklen_t client_name_len = sizeof(client_name);
	pthread_t newthread;

	server_sock = startup(&port);
	printf("TinyHttpd server running on port %d\n", port);

	while (1) {
		client_sock = accept(server_sock,
			(struct sockaddr*)&client_name,
			&client_name_len);
		if (client_sock == -1) {
			error_die("accept");
		}
		if (pthread_create(&newthread, NULL, (void*)accept_request,
			(void*)(intptr_t)client_sock) != 0) {
			error_die("pthread_create");
		}
	}

	close(server_sock);

	return 0;
}

void error_die(const char* sc) {
	perror(sc);
	exit(1);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket */
 /**********************************************************************/
int startup(u_short* port) {
	int httpd = 0;
	int on = 1;
	struct sockaddr_in name;
	httpd = socket(PF_INET, SOCK_STREAM, 0);
	if (httpd == -1) {
		error_die("socket");
	}

	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0) {
		error_die("setsockopt failed");
	}

	if (bind(httpd, (struct sockaddr*)&name, sizeof(name)) < 0) {
		error_die("bind");
	}

	if (*port == 0) {
		socklen_t namelen = sizeof(name);
		if (getsockname(httpd, (struct sockaddr*)&name, &namelen) == -1) {
			error_die("getsockname");
		}
		*port = ntohs(name.sin_port);
	}

	if (listen(httpd, 5) < 0) {
		error_die("listen");
	}

	return httpd;
}

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
 /**********************************************************************/
void accept_request(void* arg) {
	int client = (intptr_t)arg;
	char buf[1024];
	size_t numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;
	struct stat st;
	int cgi = 0; /* becomes tru if server decides this is a CGI program */

	char* query_string = NULL;

	numchars = get_line(client, buf, sizeof(buf));
	i = 0;	j = 0;
	while (!ISspace(buf[i]) && (i < sizeof(method) - 1)) {
		method[i] = buf[i];
		i++;
	}
	j = i;
	method[i] = '\0';

	if (strcasecmp(method, "GET") && strcasecmp(method, "POST")) {
		unimplemented(client);
		return;
	}

	if (strcasecmp(method, "POST") == 0) {
		cgi = 1;
	}

	i = 0;
	while (ISspace(buf[j]) && (j < numchars)) {
		j++;
	}
	while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars)) {
		url[i++] = buf[j++];
	}
	url[i] = '\0';

	if (strcasecmp(method, "GET") == 0) {
		query_string = url;
		while ((*query_string != '?') && (*query_string != '\0')) {
			query_string++;
		}
		if (*query_string == '?') {
			cgi = 1;
			*query_string = '\0';
			query_string++;
		}
	}

	sprintf(path, "webcontent%s", url);
	if (path[strlen(path) - 1] == '/') {
		strcat(path, "index.html");
	}
	if (stat(path, &st) == -1) {
		while (numchars > 0 && strcmp("\n", buf)) {
			/* read & discard headers */
			numchars = get_line(client, buf, sizeof(buf));
		}
		not_found(client);
	}
	else {
		if (st.st_mode & S_IFMT == S_IFDIR) {
			strcat(path, "/index.html");
		}
		// if(st.st_mode & S_IXUSR || st.st_mode & S_IXGRP || st.st_mode & S_IXOTH){
		// 	cgi = 1;
		// }		
		if (!cgi) {
			serve_file(client, path);
		}
		else {
			unimplemented(client);
		}
	}

	close(client);
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
 /**********************************************************************/
int get_line(int sock, char* buf, int size) {
	int i = 0;
	char c = '\0';
	int n;

	while ((i < size - 1) && (c != '\n')) {
		n = recv(sock, &c, 1, 0);
		if (n > 0) {
			if (c == '\r') {
				n = recv(sock, &c, 1, MSG_PEEK);
				if ((n > 0) && (c == '\n')) {
					recv(sock, &c, 1, 0);
				}
				else {
					c = '\n';
				}
			}
			buf[i] = c;
			i++;
		}
		else {
			c = '\n';
		}
	}
	buf[i] = '\0';

	return i;
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
 /**********************************************************************/
void unimplemented(int client)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client)
{
	char buf[1024];

	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
 /**********************************************************************/
void serve_file(int client, const char* filename)
{
	FILE* resource = NULL;
	int numchars = 1;
	char buf[1024];

	buf[0] = 'A'; buf[1] = '\0';
	while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
		numchars = get_line(client, buf, sizeof(buf));

	resource = fopen(filename, "r");
	if (resource == NULL)
		not_found(client);
	else
	{
		headers(client, filename);
		cat(client, resource);
	}
	fclose(resource);
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
 /**********************************************************************/
void headers(int client, const char* filename)
{
	char buf[1024];
	(void)filename;  /* could use filename to determine file type */

	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
 /**********************************************************************/
void cat(int client, FILE* resource)
{
	char buf[1024];

	fgets(buf, sizeof(buf), resource);
	while (!feof(resource))
	{
		send(client, buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), resource);
	}
}
