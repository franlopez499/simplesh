/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: LÓPEZ TOLEDO, FRANCISCO (G1.1)
 *          CAYUELA ESPI, SALVADOR (G1.1)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
 */


/*
 * Ficheros de cabecera
 */


#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <limits.h>
#include <libgen.h>
#include <math.h>


// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>


/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.19";
//int num_cd = 0;


// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16
#define NUM_INTERNAL_COMMANDS 5
#define BSIZE 1024
#define MAX_PIDS 8



// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";

const char * internal_commands[NUM_INTERNAL_COMMANDS] = {"cwd","cd","exit","psplit","bjobs"};
pid_t processes[MAX_PIDS];
/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char const* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char const* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, S_IRWXU, STDIN_FILENO);
                break;
            case '>':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU, STDOUT_FILENO);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_WRONLY|O_CREAT|O_APPEND, S_IRWXU, STDOUT_FILENO);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}


/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/
void run_cwd();
void run_exit();
void run_cd(struct execcmd *);
void run_psplit(struct execcmd *);
void run_bjobs(struct execcmd *);
void insert_process(pid_t pid);

int is_internal(char * command)
{
    for (int i = 0; i < NUM_INTERNAL_COMMANDS; i++)
    {
        if(command != NULL && !strcmp(command,internal_commands[i]))
            return 1;
    }
    return 0;
}


void run_internal_exec(struct execcmd * cmd)
{
	 char * command = cmd->argv[0];
    if(!strcmp(command,"cwd")){
        run_cwd();
    } else if(!strcmp(command,"exit")) {
		  run_exit(cmd);    
    } else if(!strcmp(command,"cd")) {
		  run_cd(cmd);    
    }else if(!strcmp(command,"psplit")){
        run_psplit(cmd);
    }else if(!strcmp(command,"bjobs")){
        run_bjobs(cmd);
    }
}

void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == NULL) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}


void run_cmd(struct cmd* cmd,struct sigaction * sa)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            	TRY(sigprocmask(SIG_BLOCK, &sa->sa_mask,NULL));
            ecmd = (struct execcmd*) cmd;

            if(is_internal(ecmd->argv[0])){
                run_internal_exec(ecmd);
                
            } 
                
            else{
                pid_t pid;
                if ((pid = fork_or_panic("fork EXEC")) == 0)
                    exec_cmd(ecmd);
                
                TRY( waitpid(pid,NULL,0) );    
            }
            	TRY(sigprocmask(SIG_UNBLOCK, &sa->sa_mask,NULL));


            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            /* Cuando se tiene que ejecutar un comando interno, 
               no se debe crear un proceso hijo. 
               No obstante, sí que se debe realizar la redirección.*/
            struct execcmd * ecmd = (struct execcmd *)rcmd->cmd;
            if(is_internal(ecmd->argv[0])){
				int stdout_copy = dup(1); 
                TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }

                run_internal_exec(ecmd);
                TRY(dup2(stdout_copy,STDOUT_FILENO)); 
            }else{
		        TRY(sigprocmask(SIG_BLOCK, &sa->sa_mask,NULL));
                pid_t pid;
                 if ((pid = fork_or_panic("fork REDR")) == 0)
                {
                TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }

                if (rcmd->cmd->type == EXEC){
                    exec_cmd((struct execcmd*) rcmd->cmd);
                }else
                    run_cmd(rcmd->cmd,sa);
                exit(EXIT_SUCCESS);
                }
                TRY( waitpid(pid,NULL,0) );
		        TRY(sigprocmask(SIG_UNBLOCK, &sa->sa_mask,NULL));
            }
           
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            run_cmd(lcmd->left,sa);
            run_cmd(lcmd->right,sa);
            break;

        case PIPE:
            pcmd = (struct pipecmd*)cmd;
            TRY(sigprocmask(SIG_BLOCK, &sa->sa_mask,NULL));
            
            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
			pid_t pid_left,pid_right;
            // Ejecución del hijo de la izquierda
            if ((pid_left = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(STDOUT_FILENO) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->left->type == EXEC){
                	struct execcmd * ecmd = (struct execcmd *)pcmd->left;
                	if(is_internal(ecmd->argv[0])){
							run_internal_exec(ecmd);
					}else {
						exec_cmd((struct execcmd*) pcmd->left);
					}
                
                }
                	  
                else
                    run_cmd(pcmd->left,sa);
                exit(EXIT_SUCCESS);
            }

            // Ejecución del hijo de la derecha
            
            if ((pid_right = fork_or_panic("fork PIPE right")) == 0){
                TRY( close(STDIN_FILENO) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->right->type == EXEC){
					struct execcmd * ecmd = (struct execcmd *)pcmd->right;
                	if(is_internal(ecmd->argv[0])){
						run_internal_exec(ecmd);
					}else
                        exec_cmd((struct execcmd*) pcmd->right);                
                }
                else
                    run_cmd(pcmd->right,sa);
                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( waitpid(pid_left,NULL,0) );
            TRY( waitpid(pid_right,NULL,0) );
            TRY(sigprocmask(SIG_UNBLOCK, &sa->sa_mask,NULL));

            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            pid_t pid;
            if ((pid = fork_or_panic("fork BACK")) == 0)
            {
                if (bcmd->cmd->type == EXEC) {
                	struct execcmd * ecmd = (struct execcmd *)bcmd->cmd;
                	if(is_internal(ecmd->argv[0])){
								run_internal_exec(ecmd);
                	}else
                    exec_cmd((struct execcmd*) bcmd->cmd);
                }
                else
                    run_cmd(bcmd->cmd,sa);
                exit(EXIT_SUCCESS);
            }
            
            
            insert_process(pid);
            printf("[%d]\n",pid); // Indicamos que empieza el proceso con su [PID]
            fflush(stdout);

            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            pid_t pids;
		    TRY(sigprocmask(SIG_UNBLOCK, &sa->sa_mask,NULL));            
	        if ((pids = fork_or_panic("fork SUBS")) == 0)
            {
                run_cmd(scmd->cmd,sa);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pids,NULL,0) );
	TRY(sigprocmask(SIG_UNBLOCK, &sa->sa_mask,NULL));
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}


void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd()
{
    uid_t uid = getuid();
    struct passwd * passwd = getpwuid(uid);
	if(!passwd){
		perror("getpwuid");
		exit(EXIT_FAILURE);
	}

	char * user = passwd->pw_name;
	char path[PATH_MAX];
	if(!getcwd(path,PATH_MAX)){
		perror("getcwd");
		exit(EXIT_FAILURE);
	}

	char * dir = basename(path);
	char prompt[strlen(user)+strlen(dir)+4];
	sprintf(prompt,"%s@%s> ",user,dir);
   // Lee la orden tecleada por el usuario
    char * buf = readline(prompt);

    
  
    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf)
        add_history(buf);

    return buf;
}

void insert_process(pid_t pid){
    
    for(int i = 0;i < MAX_PIDS; i++ ) {
        if(processes[i] == -1){
            processes[i] = pid;
            break;
        }
    }
}


void handle_sigchld(int sig) {


    int saved_errno = errno;
    char message[14];
    pid_t pid;

    while ((pid = waitpid((pid_t)(-1), 0, WNOHANG)) > 0) 
    {
        
        sprintf(message,"[%d]\n",pid); 
        for(int i= 0; i< MAX_PIDS;i++){
            if(processes[i]==pid){  
                processes[i] = -1;
            }
        } 
        
        TRY(write(STDOUT_FILENO,message,strlen(message))); // reentrant
    }
   

    errno = saved_errno;
}

void run_cwd()
{
    	uid_t uid = getuid();
    	struct passwd * passwd = getpwuid(uid);
	if(!passwd){
		perror("getpwuid");
		exit(EXIT_FAILURE);
	}

	char * user = passwd->pw_name;
	char path[PATH_MAX];
	if(!getcwd(path,PATH_MAX)){
		perror("getcwd");
		exit(EXIT_FAILURE);
	}

	
	char prompt[strlen(path)+8];
	sprintf(prompt,"cwd: %s\n",path);
    	printf("%s",prompt);
   
}

void run_exit(struct cmd * ecmd) 
{		
	free_cmd(ecmd);
	free(ecmd);
	exit(EXIT_SUCCESS);
}

void run_cd(struct execcmd * cmd) 
{	
    static int num_cd = 0; // Para mantener estado entre llamadas
	char path[PATH_MAX];
	if(!getcwd(path,PATH_MAX)){
		perror("getcwd");
		exit(EXIT_FAILURE);
	}
    /* Si es el primer cd que se realiza en el shell 
       y trata de realizar cd - se lanzara un mensaje de error y 
       no se aumentará el numero de cd realizados*/
	char * arg1 = cmd->argv[1];
	if(!num_cd) {
		setenv("OLDPWD",path,1);	
		if(arg1 == NULL) {
			TRY(chdir(getenv("HOME"))); 
		    num_cd++;
        }else if(!strcmp(arg1,"-")){
			printf("run_cd: Variable OLDPWD no definida\n");
		}else if(cmd->argv[2] != NULL) {
			printf("run_cd: Demasiados argumentos\n");
		}else {
			
			if(chdir(arg1) == -1)
				printf("run_cd: No existe el directorio '%s'\n",arg1);
            num_cd++;	
		}	
	} else {
		if(arg1 == NULL) {
			TRY(chdir(getenv("HOME")));
		}else if(!strcmp(arg1,"-")){
			TRY(chdir(getenv("OLDPWD")));
		}else if(cmd->argv[2] != NULL) {
			printf("run_cd: Demasiados argumentos\n");
		}else {

			if(chdir(arg1) == -1){
			    printf("run_cd: No existe el directorio '%s'\n",arg1);
            }
		}
		TRY(setenv("OLDPWD",path,1));
        num_cd++;
	}
}

void process_option(char * file, int size,int l,int maxLines,int b,int maxBytes)
{   
    int fd_read,fd_write;
	if(!strcmp("stdin",file))
        fd_read = STDIN_FILENO; //Si es la entrada estandar, ponemos que vamos a leerla
	else
    {
    	if ((fd_read = open(file, O_RDONLY)) < 0) //Sino leeremos el fichero especificado
    	{
        	perror("open");
        	exit(EXIT_FAILURE);
    	}
	}
    char nombre[strlen(file)+12];

    int offset = 0;
    int written = 0; int bytes_read = 0;
    char buf[size];
    int bytes_left = maxBytes;
    int lines = maxLines;
    int no_line = 0;
    int num_psplit = 0;
    int i,j;
    int entrado = 0;
    int empezado = 1;
    while ((bytes_read = read(fd_read, buf,size))> 0)
    {   
        if(bytes_read == -1){
            perror("read");
            exit(EXIT_FAILURE);
        }
        i = 0;
        offset = 0;
	    //En caso de que hayamos puesto la opción de l
        if(l) {
	        //Mientras aún queden bytes leidos por escribir
            while(bytes_read) 
			{              
                if((lines == maxLines && entrado) || empezado) /* Por si en la primera iteracion no se llega a escribir una linea entera,
                                                                    asi evitamos que se creen archivos infinitos.*/
                {
                    empezado =0;
                    sprintf(nombre,"%s%d",file,num_psplit++);
                    if ((fd_write = open(nombre, O_WRONLY | O_CREAT |O_TRUNC  ,S_IRWXU)) < 0)
                    {   
                        perror("open");
                        exit(EXIT_FAILURE);
                    } 
                }
                /* Contamos el numero de lineas que vamos a escribir (restar a lineas restantes)*/
                for(j=0;j < bytes_read && lines;j++){
                    if(buf[offset+j]=='\n'){ // Contamos desde offset hasta offset+bytes_read
                        entrado = 1;
                        lines--;
                    }
                }
                    // Escribimos
                    int partial = 0;
                    while((written = write(fd_write,buf+offset,j-partial))>0)
					{
                            offset+=written;
                            bytes_read-=written;
                            partial+=written;
                            

                    }
                    if(written == -1){
                        perror("write");
                        exit(EXIT_FAILURE);
                    }
                    // Si no quedan lineas cerramos el archivo actual.
                    if(lines == 0)
				    {
                        lines = maxLines; 
                        TRY(fsync(fd_write));
                        TRY( close(fd_write) );
                    }

                
        
            }
   	
        }else
		{ 
            while(bytes_read)
            {
            //Si los que quedan por colocar son el número maximo a tener
                if(bytes_left == maxBytes)
                {
                    sprintf(nombre,"%s%d",file,num_psplit++);
                    if ((fd_write = open(nombre, O_WRONLY | O_CREAT |O_TRUNC ,S_IRWXU)) < 0)
                    {   
                        perror("open");
                        exit(EXIT_FAILURE);
                    } 
                }
                //Mientras queden bytes por colocar y leer
                while(bytes_left > 0 &&  bytes_read > 0)
                {
                    // ejemplo de querer escribir 2 bytes y leer 1 byte  
					//Nos aseguramos de no escribir de más
                    if(bytes_left >= bytes_read)
					{
                        i = bytes_read; //escribimos los maximos bytes posibles en cada iteracion
                    }else 
					{ 
                        i = bytes_left;
                    }
					//Vamos a escribir
                    while((written = write(fd_write,buf+offset,i))>0)
					{
                        offset+=written;
                        bytes_read-=written;
                     	i-=written;
                        bytes_left-=written;
                    }
                    if(written == -1){
                        perror("write");
                        exit(EXIT_FAILURE);
                    }
                 }
                 if(bytes_left == 0){
                    bytes_left = maxBytes;
                    TRY(fsync(fd_write));
                    TRY( close(fd_write) );
            	}
            }
        }
	}
    if(fd_read!= STDIN_FILENO)
        TRY( close(fd_read) );
}
void run_psplit(struct execcmd * cmd){
    int opt;
    optind = 1;
    int size = BSIZE;
    int lines_per_file,bytes_per_file;
    lines_per_file = bytes_per_file = 0;
    int b =0;int l = 0; int p = 0; int index = 0;
    int procs_per_file = 0;
    
    while ((opt = getopt(cmd->argc, cmd->argv, "hl:b:s:p:")) != -1) {
        switch (opt) {
            case 's':
                size = atoi(optarg);
                if(size < 1 || size > pow(2,20)){
                    printf("psplit: Opción -s no válida\n");
                    return;
                }
                break;
            case 'b':
                if(!optarg || atoi(optarg) == 0){
                    printf("psplit: Opción -b no válida, debe de establecer un tamaño en bytes\n");
                    return;                    
                }else{
                    bytes_per_file = atoi(optarg);
                    b = 1;
                }
                break;

            case 'l':
                if(!optarg || atoi(optarg) == 0){
                    printf("psplit: Opción -l no válida, debe de establecer el número de lineas\n");
                    return;                    
                }else{
                    lines_per_file = atoi(optarg);
                    l = 1;

                }
                break;
            case 'p':
                if(!optarg || atoi(optarg) == 0){
                    printf("psplit: Opción -p no válida\n");
                    return;
                 }else{
                     procs_per_file = atoi(optarg);
                     p = procs_per_file;
                 }
                break;
            case 'h':
            	printf("Uso: psplit [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n");
				printf("Opciones:\n");
				printf("-l NLINES Número máximo de líneas por fichero.\n");
				printf("-b NBYTES Número máximo de bytes por fichero.\n");
				printf("-s BSIZE  Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n");
				printf("-p PROCS  Número máximo de procesos simultáneos.\n");
				printf("-h        Ayuda\n");
				printf("\n");
				return;
                break;
            default: /* ? */
                fprintf(stderr, "Usage: %s [-l NLINES] [-b NBYTES] [-s BSIZE] [-p PROCS] [FILE1] [FILE2]...\n", cmd->argv[0]);
                return;
        }
    }
    if(l && b){
        printf("psplit: Opciones incompatibles\n");
        exit(EXIT_FAILURE);
    }
    /* Si hemos parseado todo es que no hemos especificado ficheros por 
        argumentos y debemos de coger la entrada estándar*/
    int wstatus;
    if(optind == cmd->argc){
        process_option("stdin",size,l,lines_per_file,b,bytes_per_file);
    }else{

        if(p){
            pid_t pid[procs_per_file];
            memset(pid,-1,procs_per_file * sizeof(pid[0]));
            for(int i = optind; i < cmd->argc; i++){
                if(pid[index % procs_per_file] != -1){ //cola circular
                    TRY(waitpid(pid[(index)%procs_per_file],&wstatus,WUNTRACED | WCONTINUED)); // Para reportar estatus de hijos parados y continuados
                }

                if ((pid[(index++) % procs_per_file] = fork_or_panic("fork psplit")) == 0){
                    process_option(cmd->argv[i],size,l,lines_per_file,b,bytes_per_file); // Codigo del hijo
                    exit(EXIT_SUCCESS);
                }
            }
            
           
            for(int j = 0 ; j < procs_per_file; j++){
                if(pid[j]!= -1){
                    TRY(waitpid(pid[j],&wstatus,WUNTRACED|WCONTINUED));
                }
            }
            
        }else {
            for(int i = optind; i < cmd->argc; i++){
                process_option(cmd->argv[i],size,l,lines_per_file,b,bytes_per_file);
            }
        }
        
    }
    optind = 1;
}


void run_bjobs(struct execcmd * cmd){


    int opt;
    optind = 0;  // bug libreria getopt()
    int k,h;
    k=0;h=0;
    int i = 0;
    while ((opt = getopt(cmd->argc, cmd->argv, "hk")) != -1) {
        switch (opt) {
            case 'h':
                h=1;
                break;
            case 'k':
                k=1;
                break;
            default:
                return;
        }
    }
    
    // Aqui enviar procesos en segundo plano activos
    if(!k && !h){
        for(int i = 0;i<MAX_PIDS;i++){
            if(processes[i] != -1){
                printf("[%d]\n",processes[i]);
            }
        }

    }else if (k){

         for(int i = 0;i<MAX_PIDS;i++){
            if(processes[i]!= -1){
                TRY(kill(processes[i],SIGTERM));
            }
        }
    }else{
        printf("Uso : bjobs [ -k] [-h]\n");
        printf("      Opciones :\n");
        printf("      -k Mata todos los procesos en segundo plano.\n");
        printf("      -h Ayuda\n");
    }

    
    optind = 0;



}

/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}


int main(int argc, char** argv)
{
    struct sigaction sa;
    sa.sa_handler = &handle_sigchld;
    TRY(sigemptyset(&sa.sa_mask));
    TRY(sigaddset(&sa.sa_mask,SIGCHLD));
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, 0) == -1) {
        perror(0);
        exit(EXIT_FAILURE);
    }
    


    /* Ignore signal SIGQUIT (CTRL-ALTGR-\) */
    struct sigaction s;
    s.sa_handler = SIG_IGN;
    TRY(sigemptyset(&s.sa_mask));
    //s.sa_flags = SA_RESTART;
    if(sigaction(SIGQUIT, &s, NULL) == -1){
        perror("sigaction SIGQUIT");
        exit(EXIT_FAILURE);
    }
    

    /* Block signal SIGINT (CTRL-C) */
    sigset_t blocked_signals;
    TRY(sigemptyset(&blocked_signals));
    TRY(sigaddset(&blocked_signals, SIGINT));
    if (sigprocmask(SIG_BLOCK, &blocked_signals, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }

    char* buf;
    struct cmd* cmd;
    memset(processes,-1,MAX_PIDS * sizeof(processes[0]));
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");
	 // Eliminamos la variable de entorno OLDPWD    
    TRY(unsetenv("OLDPWD"));
    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL)
    {

        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        
        run_cmd(cmd,&sa);

        // Libera la memoria de las estructuras `cmd`
        free_cmd(cmd);

			//Terminamos de liberar la memoria de la estructura 'cmd'
        free(cmd);
        // Libera la memoria de la línea de órdenes
        free(buf);
        
        
    }
    

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}
