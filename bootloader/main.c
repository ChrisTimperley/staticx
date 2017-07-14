#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <libtar.h>
#include <sys/wait.h>
#include "error.h"
#include "mmap.h"
#include "util.h"
#include "xz.h"

#define ARCHIVE_SECTION         ".staticx.archive"
#define INTERP_FILENAME         ".staticx.interp"
#define PROG_FILENAME           ".staticx.prog"

#ifdef DEBUG
#define debug_printf(fmt, ...)   fprintf(stderr, fmt, ##__VA_ARGS__)
#define TAR_DEBUG_OPTIONS		(TAR_VERBOSE)
#else
#define debug_printf(fmt, ...)
#define TAR_DEBUG_OPTIONS		0
#endif

#define Elf_Ehdr    Elf64_Ehdr
#define Elf_Phdr    Elf64_Phdr
#define Elf_Shdr    Elf64_Shdr
#define Elf_Dyn     Elf64_Dyn


/* Our "home" directory, where the archive is extracted */
static const char *m_homedir;


static inline void *
ptr_add(void *p, size_t off)
{
    return ((uint8_t *)p) + off;
}

static inline const void *
cptr_add(const void *p, size_t off)
{
    return ((const uint8_t *)p) + off;
}

static bool
elf_is_valid(const Elf_Ehdr *ehdr)
{
    return (ehdr->e_ident[EI_MAG0] == ELFMAG0)
        && (ehdr->e_ident[EI_MAG1] == ELFMAG1)
        && (ehdr->e_ident[EI_MAG2] == ELFMAG2)
        && (ehdr->e_ident[EI_MAG3] == ELFMAG3);
}

static Elf_Phdr *
elf_get_proghdr_by_type(Elf_Ehdr *ehdr, unsigned int ptype)
{
    /* Pointer to the program header table */
    Elf_Phdr *phdr_table = ptr_add(ehdr, ehdr->e_phoff);

    /* Sanity check on size of Elf_Phdr */
    if (ehdr->e_phentsize != sizeof(Elf_Phdr))
        error(2, 0, "ELF file disagrees with program header size: %d != %zd",
            ehdr->e_phentsize, sizeof(Elf_Phdr));

    for (int i=0; i < ehdr->e_phnum; i++) {
        Elf_Phdr *ph = &phdr_table[i];

        if (ph->p_type == ptype)
            return ph;
    }
    return NULL;
}

#define SHT_NOT_USED    (SHT_HIUSER + 1)

static Elf_Shdr *
elf_get_section(Elf_Ehdr *ehdr, const char *lookup_name, Elf64_Word lookup_type)
{
    /* Pointer to the section header table */
    Elf_Shdr *shdr_table = ptr_add(ehdr, ehdr->e_shoff);

    /* Pointer to the string table section header */
    Elf_Shdr *sh_strtab = &shdr_table[ehdr->e_shstrndx];

    /* Pointer to the string table data */
    char *strtab = ptr_add(ehdr, sh_strtab->sh_offset);

    /* Sanity check on size of Elf_Shdr */
    if (ehdr->e_shentsize != sizeof(Elf_Shdr))
        error(2, 0, "ELF file disagrees with section size: %d != %zd",
            ehdr->e_shentsize, sizeof(Elf_Shdr));

    /* Iterate sections */
    debug_printf("Sections:\n");
    for (int i=0; i < ehdr->e_shnum; i++) {
        Elf_Shdr *sh = &shdr_table[i];
        const char *sh_name = strtab + sh->sh_name;

        debug_printf("[%d] %s type=0x%lX  offset=0x%lX\n",
                i, sh_name, (unsigned long)sh->sh_type, sh->sh_offset);

        /* Look up by name */
        if (lookup_name) {
            if (strcmp(sh_name, lookup_name) == 0)
                return sh;
        }

        /* Look up by type */
        if (lookup_type != SHT_NOT_USED) {
            if (sh->sh_type == lookup_type)
                return sh;
        }
    }
    return NULL;
}

static Elf_Shdr *
elf_get_section_by_name(Elf_Ehdr *ehdr, const char *lookup_name)
{
    return elf_get_section(ehdr, lookup_name, SHT_NOT_USED);
}

#if 0
static Elf_Shdr *
elf_get_section_by_type(Elf_Ehdr *ehdr, unsigned long lookup_type)
{
    return elf_get_section(ehdr, NULL, lookup_type);
}
#endif

static char *
path_join(const char *p1, const char *p2)
{
    char *result;
    if (asprintf(&result, "%s/%s", p1, p2) < 0)
        error(2, 0, "Failed to allocate path string");
    return result;
}

/******************************************************************************/
/*** TODO: Move to own c file */


/**
 * libtar is weird and uses an integer as the 'context' for its tartype_t
 * function pointers, so we can't use a void * to refer to a dynamic structure.
 * That's not a big deal, since we only care about a single instance anyway.
 * So we'll use a "fake" file descriptor number whose value doesn't matter
 * because it will never be passed to any system calls. We do this just to keep
 * libtar in check.
 */
#define XZ_FAKE_FD   42

#define XZ_DICT_MAX     8<<20       /* 8 MiB */

static struct xz_dec *m_xzdec = NULL;
static struct xz_buf m_xzbuf;

static const char * xzret_to_str(enum xz_ret r)
{
    switch (r) {
        case XZ_OK:                 return "XZ_OK";
        case XZ_STREAM_END:         return "XZ_STREAM_END";
        case XZ_UNSUPPORTED_CHECK:  return "XZ_UNSUPPORTED_CHECK";
        case XZ_MEM_ERROR:          return "XZ_MEM_ERROR";
        case XZ_MEMLIMIT_ERROR:     return "XZ_MEMLIMIT_ERROR";
        case XZ_FORMAT_ERROR:       return "XZ_FORMAT_ERROR";
        case XZ_OPTIONS_ERROR:      return "XZ_OPTIONS_ERROR";
        case XZ_DATA_ERROR:         return "XZ_DATA_ERROR";
        case XZ_BUF_ERROR:          return "XZ_BUF_ERROR";
        default:                    return "XZ_??????";
    }
}

static int xz_open(const char *pathname, int oflags, ...)
{
    m_xzdec = xz_dec_init(XZ_DYNALLOC, XZ_DICT_MAX);
    if (!m_xzdec) {
        error(2, 0, "Failed to initialize xz decoder");
        return -1;
    }

    return XZ_FAKE_FD;
}

static int xz_close(int fd)
{
    if (fd != XZ_FAKE_FD) {
        debug_printf("Unexpected fd %d\n", fd);
        return -1;
    }

    if (m_xzdec) {
        xz_dec_end(m_xzdec);
        m_xzdec = NULL;
    }

    return 0;
}

static ssize_t xz_read(int fd, void * const buf, size_t const len)
{
    /* Decompress into given output buffer */
    m_xzbuf.out      = buf;
    m_xzbuf.out_pos  = 0;
    m_xzbuf.out_size = len;

    /* Always attempt to fill the given output buffer */
    while (m_xzbuf.out_pos != m_xzbuf.out_size) {

        /* Run! */
        enum xz_ret xr = xz_dec_run(m_xzdec, &m_xzbuf);
        switch (xr) {
            case XZ_OK:
                continue;

            case XZ_STREAM_END:
                /* Return 0 to indicate EOF */
                return 0;

            default:
                error(2, 0, "xz_dec_run returned %s (%d)\n", xzret_to_str(xr), xr);
                return -1;
        }
    }

    return len;
}

static tartype_t xztype = {
    .openfunc   = xz_open,
    .closefunc  = xz_close,
    .readfunc   = xz_read,
};

static void
extract_archive(void)
{
    /* mmap this ELF file */
    struct map *map = mmap_file("/proc/self/exe", true);

    /* Find the .staticx.archive section */
    Elf_Ehdr *ehdr = map->map;
    if (!elf_is_valid(ehdr))
        error(2, 0, "Invalid ELF header");

    const Elf_Shdr *shdr = elf_get_section_by_name(ehdr, ARCHIVE_SECTION);
    if (!shdr)
        error(2, 0, "Failed to find "ARCHIVE_SECTION" section");


    size_t tarxz_size = shdr->sh_size;
    const void *tarxz_data = cptr_add(ehdr, shdr->sh_offset);

    m_xzbuf = (typeof(m_xzbuf)) {
        .in      = tarxz_data,
        .in_pos  = 0,
        .in_size = tarxz_size,
        /* Other fields initialized to zero */
    };

    TAR *t;
    errno = 0;
    if (tar_open(&t, "", &xztype, O_RDONLY, 0, TAR_DEBUG_OPTIONS) != 0)
        error(2, errno, "tar_open() failed");

    /* XXX Why is it so hard for people to use 'const'? */
    if (tar_extract_all(t, (char*)m_homedir) != 0)
        error(2, errno, "tar_extract_all() failed");

    if (tar_close(t) != 0)
        error(2, errno, "tar_close() failed");
    t = NULL;
    debug_printf("Successfully extracted archive to %s\n", m_homedir);

    unmap_file(map);
    map = NULL;
}

static void
set_interp(Elf_Ehdr *ehdr, const char *new_interp)
{
    /* Find the interpreter string */
    Elf_Phdr *ph = elf_get_proghdr_by_type(ehdr, PT_INTERP);
    if (!ph)
        error(2, 0, "Failed to find PT_INTERP header");

    /* Make sure it is NUL terminated */
    char *interp = ptr_add(ehdr, ph->p_offset);
    size_t interp_size = ph->p_filesz;

    if (interp[interp_size - 1] != '\0')
        error(2, 0, "Current INTERP not NUL terminated");

    debug_printf("Current program interpreter: \"%s\"\n", interp);

    if (strlen(new_interp) > interp_size - 1)
        error(2, 0, "Current INTERP too small");

    strcpy(interp, new_interp);
    debug_printf("Set new interpreter: \"%s\"\n", new_interp);
}

static void
set_rpath(Elf_Ehdr *ehdr, const char *new_rpath)
{
    /* Find the dynamic section */
    Elf_Shdr *dyn_sh = elf_get_section_by_name(ehdr, ".dynamic");
    if (!dyn_sh)
        error(2, 0, "Failed to find .dynamic section");

    /* Base and size of dynamic table */
    Elf_Dyn *dyn_table = ptr_add(ehdr, dyn_sh->sh_offset);
    size_t ndyn = dyn_sh->sh_size / sizeof(Elf_Dyn);


    /* Find the dynamic string section */
    /* Technically I think we should use DT_STRTAB, DT_STRSZ, but those are
     * *addresses*, and not file offsets */
    Elf_Shdr *dynstr_sh = elf_get_section_by_name(ehdr, ".dynstr");
    if (!dynstr_sh)
        error(2, 0, "Failed to find .dynamic section");


    /* Setup pointer to dynamic string table */
    char *dynstrtab = ptr_add(ehdr, dynstr_sh->sh_offset);
    size_t dynstrsz = dynstr_sh->sh_size;
    debug_printf("Dynamic string table: offset=0x%lX size=0x%lX\n",
            dynstr_sh->sh_offset, dynstrsz);

    /* Find needed dynamic tags */
#if 0
    Elf_Dyn *dt_strtab = NULL;  /* DT_STRTAB */
    Elf_Dyn *dt_strsz  = NULL;  /* DT_STRSZ */
#endif
    Elf_Dyn *dt_rpath  = NULL;  /* DT_RPATH */
    debug_printf("Dynamic tags:\n");
    for (size_t i = 0; i < ndyn; i++) {
        Elf_Dyn *dt = &dyn_table[i];
        debug_printf("0x%lX (%ld): 0x%lX\n", dt->d_tag, dt->d_tag, dt->d_un.d_val);

        switch (dt->d_tag) {
            case DT_NULL:
                goto dyn_done;
#if 0
            case DT_STRTAB:
                dt_strtab = dt;
                break;
            case DT_STRSZ:
                dt_strsz = dt;
                break;
#endif
            case DT_RPATH:
                dt_rpath = dt;
                break;
        }
    }
dyn_done:

#if 0
    if (!dt_strtab)
        error(2, 0, "Couldn't find DT_STRTAB tag");
    if (!dt_strsz)
        error(2, 0, "Couldn't find DT_STRSZ tag");
#endif
    if (!dt_rpath)
        error(2, 0, "Couldn't find DT_RPATH tag");


    /* Find RPATH */
    if (dt_rpath->d_un.d_val > dynstrsz)
        error(2, 0, "RPATH outside of dynamic strtab!");
    char *rpath = ptr_add(dynstrtab, dt_rpath->d_un.d_val);

    debug_printf("Current RPATH (0x%lX):\n", dt_rpath->d_un.d_val);
    debug_printf("\"%s\"\n", rpath);

    /* Set new RPATH */
    if (strlen(new_rpath) > strlen(rpath))
        error(2, 0, "Current RPATH too small");

    strcpy(rpath, new_rpath);
    debug_printf("Set new RPATH: \"%s\"\n", new_rpath);
}


static void
patch_prog_paths(const char *prog_path, const char *new_interp, const char *new_rpath)
{
    /* mmap the prog */
    struct map *map = mmap_file(prog_path, false);

    Elf_Ehdr *ehdr = map->map;
    if (!elf_is_valid(ehdr))
        error(2, 0, "Invalid ELF header");

    set_interp(ehdr, new_interp);
    set_rpath(ehdr, new_rpath);

    unmap_file(map);
    map = NULL;
}

static void
patch_app(const char *prog_path)
{
    char *interp_path = path_join(m_homedir, INTERP_FILENAME);
    const char *new_rpath = m_homedir;

    patch_prog_paths(prog_path, interp_path, new_rpath);

    free(interp_path);
}

static char *
create_tmpdir(void)
{
    static char template[] = "/tmp/staticx-XXXXXX";
    char *tmpdir = mkdtemp(template);
    if (!tmpdir)
        error(2, errno, "Failed to create tempdir");
    return tmpdir;
}

static char **
make_argv(int orig_argc, char **orig_argv, char *argv0)
{
    /**
     * Generate an argv to execute the user app:
     */
    int len = 1 + (orig_argc-1) + 1;
    char **argv = calloc(len, sizeof(char*));

    int w = 0;
    argv[w++] = argv0;

    for (int i=1; i < orig_argc; i++) {
        argv[w++] = orig_argv[i];
    }
    argv[w++] = NULL;
    assert(w == len);

    return argv;
}

static pid_t child_pid;

static void sig_handler(int signum)
{
    /* Forward received signal to child */
    debug_printf("Forwarding signal %d to child %d\n", signum, child_pid);
    kill(child_pid, signum);
}

static void
setup_sig_handler(int signum)
{
    struct sigaction sa = {
        .sa_handler = sig_handler,
    };
    sigemptyset(&sa.sa_mask);

    if (sigaction(signum, &sa, NULL) < 0)
        error(2, errno, "Error establishing handler for signal %d", signum);
}

static void
restore_sig_handler(int signum)
{
    struct sigaction sa = {
        .sa_handler = SIG_DFL,
    };
    sigemptyset(&sa.sa_mask);

    if (sigaction(signum, &sa, NULL) < 0)
        error(2, errno, "Error restoring handler for signal %d", signum);
}

/**
 * Run the user application in a child process.
 *
 * Returns the child wait status
 */
static int
run_app(int argc, char **argv, char *prog_path)
{
    /* Generate argv for child app */
    char **new_argv = make_argv(argc, argv, prog_path);

    debug_printf("New argv:\n");
    for (int i=0; ; i++) {
        char *a = new_argv[i];
        if (!a) break;

        debug_printf("[%d] = \"%s\"\n", i, a);
    }

    /* Create new process */
    child_pid = fork();
    if (child_pid < 0)
        error(2, errno, "Failed to fork child process");


    if (child_pid == 0) {
        /*** Child ***/
        debug_printf("child: Born\n");

        execv(new_argv[0], new_argv);

        fprintf(stderr, "Failed to execv() %s: %m\n", new_argv[0]);
        _exit(3);
    }

    /*** Parent ***/

    /* Forward terminating signals to child */
    setup_sig_handler(SIGINT);
    setup_sig_handler(SIGTERM);
    /* SIGKILL can't be caught */


    /* Wait for child to exit */
    int wstatus;
    while (waitpid(child_pid, &wstatus, 0) < 0) {
        if (errno == EINTR)
            continue;
        error(2, errno, "Failed to wait for child process %ld", child_pid);
    }
    child_pid = 0;

    /* Restore signal handlers */
    restore_sig_handler(SIGINT);
    restore_sig_handler(SIGTERM);

    return wstatus;
}

int
main(int argc, char **argv)
{
    xz_crc32_init();

    /* Create temporary directory where archive will be extracted */
    m_homedir = create_tmpdir();
    debug_printf("Home dir: %s\n", m_homedir);

    /* Extract the archive embedded in this program */
    extract_archive();

    /* Get path to user application inside temp dir */
    char *prog_path = path_join(m_homedir, PROG_FILENAME);

    /* Patch the user application ELF to run in the temp dir */
    patch_app(prog_path);

    /* Run the user application */
    int wstatus = run_app(argc, argv, prog_path);

    free(prog_path);
    prog_path = NULL;

    /* Cleanup */
    debug_printf("Removing temp dir %s\n", m_homedir);
    if (remove_tree(m_homedir) < 0) {
        fprintf(stderr, "staticx: Failed to cleanup %s: %m\n", m_homedir);
    }
    m_homedir = NULL;

    /* Did child exit normally? */
    if (WIFEXITED(wstatus)) {
        int code = WEXITSTATUS(wstatus);
        debug_printf("Child exited with status %d\n", code);
        return code;
    }

    /* Did child exit due to signal? */
    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        debug_printf("Child terminated due to signal %d\n", sig);

        /* Send the same signal to ourselves */
        raise(sig);
    }

    /* Unexpected case! */
    error(2, 0, "Child exited for unknown reason! (wstatus == %d)", wstatus);

    return 2;   // Make GCC happy
}
