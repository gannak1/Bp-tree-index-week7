#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char *argv[])
{
    int *asd = NULL;
    char a[] = "a,b,c,d";
    char *p = a;
    char *c[1];
    for (int i = 0; i < 4; i++)
    {
        c[0] = p;
        p = strchr(p, ',');
        *p='\0';
        p++;
        printf("%s\n", *c);
    }
}