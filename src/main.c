#include "lab.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef TEST
#define main main_exclude
#endif



int main(int argc, char *argv[])
{
    

    //usage message displayed to user
    char *useMsg = "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
                       "         [-H helo-host] <server>\n"
                       "\n"
                       "-f <from>       envelope sender, for example you@example.com\n"
                       "-t <to>         envelope recipient\n"
                       "-s <subject>    subject line (default: empty)\n"
                       "-b <body>       message body (default: read from stdin)\n"
                       "-p <port>       port or service name (default: 25)\n"
                       "-H <helo-host>  host name sent with HELO (default: localhost)\n"
                       "<server>        host name or address of the mail server\n";
    
    // printf("%s\n", useMsg);
    return 0;
}