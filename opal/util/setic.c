//#include "opal_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include <unistd.h>
#include <ctype.h>
#include <dlfcn.h>
#include <infiniband/verbs.h>
#include "opal/util/setic.h"

#include "opal/mca/pmix/base/base.h"

char *hpmp_module_list = 0; // ends up as a long string of the form :foo:bar:etc:
char **hpmp_pci_list = 0; // array of lines

void hpmp_ic_lsmod_init(void);
void hpmp_ic_lsmod_free(void);

// in place of 'lsmod' use list of /sys/module/*/initstate
void
hpmp_ic_lsmod_init(void) {
    struct stat stbuf;
    int i, n;
    DIR *dir;
    struct dirent *entry;
    char fname[256];

    if (hpmp_module_list) {
        return;
    }
    hpmp_module_list = malloc(8192);
    if (!hpmp_module_list) {
        printf("malloc failed\n");
        exit(1);
    }

    strcpy(hpmp_module_list, ":");
    dir = opendir("/sys/module");
    while (dir && (entry = readdir(dir)) && strlen(entry->d_name) < 128) {
        if (0 == strcmp(entry->d_name, ".")) { continue; }
        if (0 == strcmp(entry->d_name, "..")) { continue; }
        if (strlen(hpmp_module_list) + strlen(entry->d_name) + 10 > 8192) { break; }
        sprintf(fname, "%s/%s/initstate", "/sys/module", entry->d_name);
        if (0 == stat(fname, &stbuf)) {
            strcat(hpmp_module_list, entry->d_name);
            strcat(hpmp_module_list, ":");
        }
    }
    if (dir) { closedir(dir); }

    n = strlen(hpmp_module_list);
    for (i=0; i<n; ++i) {
        hpmp_module_list[i] = tolower(hpmp_module_list[i]);
    }
}
void
hpmp_ic_lsmod_free(void) {
    if (hpmp_module_list) {
        free(hpmp_module_list);
        hpmp_module_list = 0;
    }
}
/*
 *  Return 1 if the str pattern is in hpmp_module_list
 *  If the modules in the list are foo bar etc that would
 *  be in the string as :foo:bar:etc:
 *  here if you do LOOKUP_MODULE("foo") it looks for :foo: in the above
 */
static int
LOOKUP_MODULE(char *str)
{
    char *searchstr;
    int rv, i, n;
    hpmp_ic_lsmod_init();
    if (!hpmp_module_list) {
        return(0);
    }
    n = (int)strlen(str);
    searchstr = malloc(n + 3);
    if (!searchstr) {
        return(0);
    }
    searchstr[0] = ':';
    searchstr[n+1] = ':';
    searchstr[n+2] = 0;
    for (i=0; i<n; ++i) {
        searchstr[i+1] = tolower(str[i]);
    }

    if (strstr(hpmp_module_list, searchstr)) {
        rv = 1;
    } else {
        rv = 0;
    }

    free(searchstr);
    return(rv);
}
// The two big pieces of identifying a PCI devices list are
// 1. walk /sys/bus/pci/devices/* looking at /vendor and /devices
//    which are files containing a hex id
// 2. a big section of /usr/share/hwdata/pci.ids goes into pci_lines[]
//    that is used to get strings represented by the vendor:device hex
#define DEVICES_DIR "/sys/bus/pci/devices"
#define PCI_FILE "/usr/share/hwdata/pci.ids"

static int
read_hex_from_pci_file(char *fname, char *out) {
    FILE *fp;
    char str[64];
    fp = fopen(fname, "r");
    if (fp && fgets(str, 64, fp)) {
        char *p = str;
        if (0 == strncmp(p, "0x", 2)) {
            p += 2;
        }
        int len = strlen(p);
        char lastchar = p[len-1];
        while (len >= 1 && !
            (
                ('a' <= lastchar && lastchar <= 'f') ||
                ('A' <= lastchar && lastchar <= 'F') ||
                ('0' <= lastchar && lastchar <= '9')
            ))
        {
            p[len-1] = 0;
            --len;
            if (len >= 1) { lastchar = p[len-1]; }
        }
        strcpy(out, p);
    }
    if (fp) { fclose(fp); }
    return 0;
}
static int
read_pci_lines(char ***lines)
{
    FILE *fp;
    char str[256];
    int nlines;
// first pass get a max of how many lines we might need:
    nlines = 1;
    fp = fopen(PCI_FILE, "r");
    while (fp && fgets(str, 256, fp)) {
        ++nlines;
    }
    if (fp) { fclose(fp); }
    (*lines) = (char**) malloc(nlines * sizeof(char*));
// now read into the various (*lines)[]
    nlines = 0;
    fp = fopen(PCI_FILE, "r");
// read past 1. ^#.*Syntax:
//           2. ^# *vendor *vendor_name
// keep lines 1. ^hex *
//            2. ^\thex *
// stop when reach next ^#.*Syntax:
//
// state:
//   0 = haven't reached first syntax line
//   1 = saw syntax, haven't seen vendor vendor
//   2 = saw vendor vendor, haven't seen second syntax yet (keep lines in this state)
//   3 = saw second syntax
    int state = 0;
    while (fp && fgets(str, 256, fp)) {
        if (state == 0 && str[0] == '#' && strstr(str, "Syntax:")) { state = 1; }
        if (state == 1 && str[0] == '#' && strstr(str, " vendor ")
            && strstr(str, " vendor_name"))
        {
            state = 2;
        }
        if (state == 2 && str[0] == '#' && strstr(str, "Syntax:")) { state = 3; }

        int len = strlen(str);
        if (len >= 1) { str[len-1] = 0; } // removing \n on the end

        // skip over uninteresting lines
        if (str[0] == 0) { continue; }
        if (str[0] == '#') { continue; }
        if (str[0] == '\t' && str[1] == '\t') { continue; }

        if (state == 2) {
            (*lines)[nlines] = malloc(strlen(str) + 1);
            strcpy((*lines)[nlines], str);
            ++nlines;
        }
    }
    if (fp) { fclose(fp); }
    (*lines)[nlines] = 0;
    return 0;
}
// pci_lines might contain
// 15b3  Mellanox Technologies
//       1019  MT28800 Family [ConnectX-5 Ex]
// so given 15b3 and 1019, construct
//   Mellanox Technologies MT28800 Family [ConnectX-5 Ex]
static int
build_pci_string(char **pci_lines, char *vendor, char *device, char *str)
{
    char *p;
    int i;
    int in_vendor_section = 0;

    for (i=0; pci_lines[i]; ++i) {
        p = pci_lines[i];
        if (p[0] != '\t') {
            in_vendor_section = 0;
            if (0 == strncmp(p, vendor, strlen(vendor)) && p[strlen(vendor)] == ' ') {
                in_vendor_section = 1;
                p += strlen(vendor);
                while (*p == ' ') { ++p; }
                strcpy(str, p);
                strcat(str, " ");
            }
        }
        if (in_vendor_section && p[0] == '\t') {
            ++p;
            if (0 == strncmp(p, device, strlen(device)) && p[strlen(device)] == ' ') {
                p += strlen(device);
                while (*p == ' ') { ++p; }
                strcat(str, p);
                return 0;
            }
        }
    }
    str[0] = 0;
    return -1;
}
void hpmp_ic_lspci_init(void);
void
hpmp_ic_lspci_init(void) {
    int i;
    DIR *dir;
    struct dirent *entry;
    char **pci_lines; // relevant lines from /usr/share/hwdata/pci.ids
    char vendor_str[64];
    char device_str[64];
    char fname[256];
    char str[256];

    if (hpmp_pci_list) {
        return;
    }

// First pass get an upper bound for how long hpmp_pci_list[] might be
    int nlines = 1;
    dir = opendir(DEVICES_DIR);
    while (dir && (entry = readdir(dir)) && strlen(entry->d_name) < 128) {
        if (0 == strcmp(entry->d_name, ".")) { continue; }
        if (0 == strcmp(entry->d_name, "..")) { continue; }
        ++nlines;
    }
    if (dir) { closedir(dir); }

    hpmp_pci_list = malloc(nlines * sizeof(char*));
    if (!hpmp_pci_list) {
        printf("malloc failed\n");
        exit(1);
    }

    read_pci_lines(&pci_lines);

    nlines = 0;
    dir = opendir(DEVICES_DIR);
    while (dir && (entry = readdir(dir)) && strlen(entry->d_name) < 128) {
        if (0 == strcmp(entry->d_name, ".")) { continue; }
        if (0 == strcmp(entry->d_name, "..")) { continue; }
        sprintf(fname, "%s/%s/vendor", DEVICES_DIR, entry->d_name);
        read_hex_from_pci_file(fname, vendor_str);
        sprintf(fname, "%s/%s/device", DEVICES_DIR, entry->d_name);
        read_hex_from_pci_file(fname, device_str);
        build_pci_string(pci_lines, vendor_str, device_str, str);

        hpmp_pci_list[nlines] = malloc(strlen(str) + 1);
        strcpy(hpmp_pci_list[nlines], str);
        ++nlines;
    }
    if (dir) { closedir(dir); }

    for (i=0; pci_lines[i]; ++i) {
        free(pci_lines[i]);
    }
    free(pci_lines);
}
void hpmp_ic_lspci_free(void);
void
hpmp_ic_lspci_free(void) {
    int i;
    if (hpmp_pci_list) {
        for (i=0; hpmp_pci_list[i]; ++i) {
            free(hpmp_pci_list[i]);
        }
        free(hpmp_pci_list);
        hpmp_pci_list = 0;
    }
}
/*
 *  Return 1 if the str pattern is in hpmp_pci_list[]
 */
static int
LOOKUP_PCI(char *str, int ignorecase)
{
    int i, n;
    char needle[64];
    char haystack[256];

    hpmp_ic_lspci_init();
    if (!hpmp_pci_list) {
        return(0);
    }

    strcpy(needle, str);
    if (!ignorecase) {
        n = strlen(needle);
        for (i=0; i<n; ++i) {
            needle[i] = tolower(needle[i]);
        }
    }

    for (i=0; hpmp_pci_list[i]; ++i) {
        strcpy(haystack, hpmp_pci_list[i]);
        if (!ignorecase) {
            n = strlen(haystack);
            for (i=0; i<n; ++i) {
                haystack[i] = tolower(haystack[i]);
            }
        }
        if (strstr(haystack, needle)) {
            return 1;
        }
    }

    return(0);
}

#define IC_MASK_PAMI  (1<<0)
#define IC_MASK_UCX   (1<<1)
#define IC_MASK_MXM   (1<<2)
#define IC_MASK_MXMC  (1<<3)
#define IC_MASK_IBV   (1<<4)
#define IC_MASK_PSM2  (1<<5)
#define IC_MASK_PSM   (1<<6)
#define IC_MASK_USNIC (1<<7)
#define IC_MASK_TCP   (1<<8)

static int
ic_string_to_bit(char *ic_upper) {
    if (0 == strcmp("PAMI", ic_upper)) {
        return IC_MASK_PAMI;
    } else if (0 == strcmp("UCX", ic_upper)) {
        return IC_MASK_UCX;
    } else if (0 == strcmp("MXM", ic_upper)) {
        return IC_MASK_MXM;
    } else if (0 == strcmp("MXMC", ic_upper)) {
        return IC_MASK_MXMC;
    } else if (0 == strcmp("IBV", ic_upper)) {
        return IC_MASK_IBV;
    } else if (0 == strcmp("PSM2", ic_upper)) {
        return IC_MASK_PSM2;
    } else if (0 == strcmp("PSM", ic_upper)) {
        return IC_MASK_PSM;
    } else if (0 == strcmp("USNIC", ic_upper)) {
        return IC_MASK_USNIC;
    } else if (0 == strcmp("TCP", ic_upper)) {
        return IC_MASK_TCP;
    }
    return 0;
}

// parse_helper
//
// takes strings of the form
//     stuff stuff || stuff || stuff stuff stuff
// and outputs a char ***str with
//     str[0][0] = "stuff"; str[0][1] = "stuff";                      str[0][2] = NULL;
//     str[1][0] = "stuff";                                           str[1][1] = NULL;
//     str[2][0] = "stuff"; str[2][1] = "stuff"; str[2][2] = "stuff"; str[2][3] = NULL;
//     str[3] = NULL;
// allocated in an easy format: an NxN setup for the char** followed by a copy of the
// original string that all the char** point into

static void
parse_helper(char ****space, char *mainstring) {
    int i, n, sz;
    char ***data;
    char *p;

    if (!mainstring || !*mainstring) {
        *space = malloc(sizeof(char**));
        data = *space;
        data[0] = NULL;
        return;
#if 0
// handling the base case using some of the code from the general case but with n==1
// (this was unnecessary)
        n = 1;
        sz = n * sizeof(char**) + n*n*sizeof(char*);
        *space = malloc(sz);
        data = *space;
        p = (char*) *space;
        p += n * sizeof(char **);
        for (i=0; i<n; ++i) {
            data[i] = (char **) p;
            p += n * sizeof(char *);
        }
        data[0][0] = NULL;
        return;
#endif
    }

    // Figure out largest 'n' needed by any portion of the string:
    // eg, how many || chars +1  and how many space separated items within a section

    char *ch;
    int nsections, nlibs, state_onlib;
    n = 4;
    ch = mainstring;
    nsections = 0; nlibs = 0;
    state_onlib = 0;
    while (*ch) {
        if (0 == strncmp(ch, "||", 2)) {
            ch += 2;
            state_onlib = 0;
            ++nsections;
            nlibs = 0;
        } else if (*ch != ' ' && *ch !=  '\t') {
            ch += 1;
            if (!state_onlib) {
                state_onlib = 1;
                ++nlibs;
                if (nlibs > n) { n = nlibs; }
            }
        } else { // space
            ch += 1;
            state_onlib = 0;
        }
    }
    ++nsections;
    if (nsections > n) { n = nsections; }
    ++n;

    sz = n * sizeof(char**) + n*n*sizeof(char*) + strlen(mainstring) + 1;

    *space = malloc(sz);
    data = *space;

    p = (char*) *space;
    p += n * sizeof(char **);

    for (i=0; i<n; ++i) {
        data[i] = (char **) p;
        p += n * sizeof(char *);
    }

    strcpy(p, mainstring);

// walk p setting data[i][j]

    state_onlib = 0;
    nsections = nlibs = 0;
    while (*p) {
        if (0 == strncmp(p, "||", 2)) {
            *p = 0;
            state_onlib = 0;
            data[nsections][nlibs] = NULL;
            ++nsections;
            nlibs = 0;
            p += 2;
        } else if (*p != ' ' && *p !=  '\t') {
            if (!state_onlib) {
                state_onlib = 1;
                data[nsections][nlibs] = p;
                ++nlibs;
            }
            p += 1;
        } else { // space
            *p = 0;
            state_onlib = 0;
            p += 1;
        }
    }
    data[nsections][nlibs] = NULL;
    data[nsections + 1] = NULL;
    if (nlibs == 0) { data[nsections] = NULL; }
}

// This one only takes strings of the form
//     stuff stuff stuff or stuff,stuff,stuff or stuff:stuff:stuff
// and outputs a char **str with
//     str[0] = "stuff"; str[1] = "stuff"; str[2] = NULL; str[3] = NULL;
// allocated in an easy format: N char* followed by a copy of the
// original string that all the char* point into
static void
parse_helper_simple_list(char ***space, char *mainstring) {
    int n, sz;
    char **data;
    char *p;

    if (!mainstring || !*mainstring) {
        *space = malloc(sizeof(char*));
        data = *space;
        data[0] = NULL;
        return;
    }

    // Figure out largest 'n' needed for the array
    // eg, how many separator chars +1

    char *ch;
    n = 1;
    ch = mainstring;
    while (*ch) {
        if (*ch == ',' || *ch == ' ' || *ch == '\t' || *ch == ':') { ++n; }
        ++ch;
    }
    ++n; // room for the list to end with a null ptr
    sz = n * sizeof(char*) + strlen(mainstring) + 1;

    *space = malloc(sz);
    data = *space;

    p = (char*) *space;
    p += n * sizeof(char *);
    strcpy(p, mainstring);

// walk p setting data[i]

    int nsections;
    nsections = 0;
    while (*p == ',' || *p == ' ' || *p == '\t' || *p == ':') { ++p; }
    while (*p) {
        data[nsections++] = p;
        ++p;
        while (*p && !(*p == ',' || *p == ' ' || *p == '\t' || *p == ':')) { ++p; }
        if (*p) {
            *p = 0;
            ++p;
        }
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == ':') { ++p; }
    }
    data[nsections] = NULL;
}

static int ibv_cardcheck(void *handle);
static int
ibv_cardcheck(void *handle) {
    struct ibv_device       *interface;
    struct ibv_device       **interface_list;
    int                     nif;
    int                     nhca;
    int                     i;
    struct ibv_device       **(*ibv_get_device_list_p)(int *num_devices);

    *((void**)&ibv_get_device_list_p) = dlsym(handle, "ibv_get_device_list");
    if (ibv_get_device_list_p == NULL) {
        return(1);
    }

    interface_list = ibv_get_device_list_p(&nif);
    if (nif <= 0) {
        return(1);
    }
#if 0
// We used to have the below, but I'm not sure why
// I guess platform's main code would only look at the first x cards
#define MAX_HCA 8
    if (nif > MAX_HCA) nif = MAX_HCA;
#endif

    nhca = 0;
    for (i = 0; i < nif; i++) {
        interface = interface_list[i];
#ifndef _OFED_VERBS_1_0_
        if (interface->transport_type == IBV_TRANSPORT_IWARP) {
            continue;
        }
#endif
        ++nhca;
    }

    /* should we free interface_list now? */
    if (nhca) {
        return(0);
    }
    return(1);
}

typedef struct {
    int myrank;
    int nranks;

    int mylrank; // my local rank
    int nlranks;
    int myhost;
    int nhosts;
    int *leader_ranks;

    int has_rank_info; // are myrank/nranks set right?
    int has_host_info; // are mylrank/nlranks/myhost/nhosts set right?
} pmix_layout_t;
static pmix_layout_t pmix_layout;

// conf vars have setting like
//    MPI_ICLIB_IBV = libibverbs.so || libibverbs.so.1 || libfoo.so libbar.so
//    MPI_ICMOD_IBV = ib_core || foo bar
// The input to this function is "IBV" for example and we parse the above vars and
// see if we can find the modules and open the libs.
//
// Returns:        - 1 if the IC is present (for real)
//                 - 2 if we're punting due to MPI_HASIC_XX
//                 - 0 otherwise

static int
hasic(char *whichic)

{
    static int avoid_rtld_global = -1;
    static int kill_timeout = 5;
    int tooverbose = 0;
    int     i, j, rv, nlibs;
    void    *handle[64];
    int modsfound = 0;
    int libsfound = 0;
    char libs_var[256];    /* for name of env var */
    char libdirs_var[256]; /* for name of env var */
    char mods_var[256];    /* for name of env var */
    char ***libs_list;
    char ***libdirs_list;
    char ***mods_list;

    if (avoid_rtld_global == -1) {
        char *p;

        avoid_rtld_global = 0;
        p = getenv("MPI_IC_AVOID_RTLD_GLOBAL");
        if (p && atoi(p) == 1) {
            avoid_rtld_global = 1;
        }

        p = getenv("MPI_ICKILL_TIMEOUT");
        if (p && *p) {
            kill_timeout = atoi(p);
            if (kill_timeout < 1) { kill_timeout = 1; }
        }
    }

    hpmp_ic_lsmod_init();
    hpmp_ic_lspci_init();

    tooverbose = (getenv("MPI_IC_TOOVERBOSE")) ? 1 : 0;

// short circuit for MPI_HASIC_%s:
    sprintf(libs_var, "MPI_HASIC_%s", whichic);
    if (getenv(libs_var)) {
        return(2);
    }

/* figure out names of the env vars we're interested in */
    sprintf(libs_var, "MPI_ICLIB_%s", whichic);
    sprintf(libdirs_var, "MPI_ICLIB_%sDIR", whichic);
    sprintf(mods_var, "MPI_ICMOD_%s", whichic);

#if 0
/* get values for the lib list and the module list */
    if (!(getenv(libs_var) && getenv(mods_var))) {
        return(0);
    }
#endif

    parse_helper(&mods_list, getenv(mods_var));
    parse_helper(&libs_list, getenv(libs_var));
    parse_helper(&libdirs_list, getenv(libdirs_var));

#if 0
    i = 0;
    while (data[i]) {
        printf("%d:", i);
        j = 0;
        while (data[i][j]) { printf(" [%s]", data[i][j]); ++j; }
        printf("\n");
        ++i;
    }
#endif

/*
 *  Check the modules (OR across the outer pieces, AND across the inner) eg
 *  mod1 mod2 || mod3 mod4 means (1 and 2) or (3 and 4)
 *  I'm inclined not to accept degenerately true cases like "mod1 || || mod3" though
 */
    i = 0;
    modsfound = 0;
    while (mods_list[i] && !modsfound) {
        j = 0;
        modsfound = 1;
        while (mods_list[i][j] && modsfound) {
            rv = LOOKUP_MODULE(mods_list[i][j]);
            if (rv == 0) {
                modsfound = 0;
            }
            ++j;
        }
        if (j == 0) { modsfound = 0; }
        ++i;
    }
    free(mods_list);
    if (i == 0) { modsfound = 1; } // degenerately true, no modules list
// For PAMI we'll have a special case hard-coded for single-host. We don't currently have a way to
// specify a context-sensitive setting in MPI_ICMOD_PAMI to say "PAMI requires ib_core for multihost
// but not for single host" so we're relying on just hard coding that here:
    if (0 == strcmp(whichic, "PAMI") &&
        pmix_layout.nhosts == 1)
    {
        modsfound = 1;
    }

    if (!modsfound) {
        free(libs_list);
        if (libdirs_list) { free(libdirs_list); }
        if (tooverbose) printf("lsmod test could not "
            "find modules in list %s\n",
            getenv(mods_var));
        return(0);
    }
/*
 *  Check if we can dlopen the libraries (same as above, OR across the outer pieces,
 *  AND across the inner)
 *  handle[0] .. handle[nlibs-1] end up being dlopened at the same time
 */
    i = 0;
    libsfound = 0;
    nlibs = 0;
    while (libs_list[i] && !libsfound) {
        j = 0;
        libsfound = 1;
        while (libs_list[i][j] && libsfound) {
            int mode = RTLD_NOW;
            if (!avoid_rtld_global) { mode |= RTLD_GLOBAL; }
            if (libs_list[i][j+1]) { mode |= RTLD_GLOBAL; }
            handle[j] = (void*)dlopen(libs_list[i][j], mode);
// always check in $MPI_ROOT/lib/openmpi/%s too:
// This makes it possible to dlopen things like mca_pml_foo.so
// (That "openmpi" string ought to be configurable though.)
            if (!handle[j] && (getenv("MPI_ROOT") || getenv("OPAL_PREFIX"))) {
                    char *libname, *root;
                    root = getenv("MPI_ROOT");
                    if (!root) { root = getenv("OPAL_PREFIX"); }
                    libname = malloc(strlen(root) + 64 + strlen(libs_list[i][j]));
                    sprintf(libname, "%s/lib/openmpi/%s", root, libs_list[i][j]);
                    handle[j] = (void *)dlopen(libname, mode);
                    free(libname);
            }
            // look through the libdirs list as well if the default search path didn't find it
            if (!handle[j] && libdirs_list[0] && libs_list[i][j][0]!='/') {
                int k = 0;
                while (libdirs_list[0][k] && !handle[j]) {
                    char *libname;
                    libname = malloc(strlen(libdirs_list[0][k]) + 1 + strlen(libs_list[i][j]) + 1);
                    sprintf(libname, "%s/%s", libdirs_list[0][k], libs_list[i][j]);
                    handle[j] = (void *)dlopen(libname, mode);
                    free(libname);
                    ++k;
                }
            }
            if (!handle[j]) {
                libsfound = 0;
            } else {
                nlibs = ++j;
                if (nlibs >= 64) {
                    fprintf(stderr, "Warning: "
                            "too many %s libraries", whichic);
                    j = 0;
                }
            }
        }
        if (j == 0) { libsfound = 0; }

        // close the open portion of the libs if we didn't find them all
        if (!libsfound) {
            for (j = nlibs - 1; j >= 0; j--) {
                dlclose(handle[j]);
            }
        }

        ++i;
    }
    free(libs_list);
    free(libdirs_list);
    if (i == 0) { libsfound = 1; } // degenerately true, no libs list

/*
 *  Special extra check for ibv, while the library is still open,
 *  actually call some functions from the ibv device to see if it's
 *  going to work.
 */
    int extra_ibv_check = 0;
    if (0 == strcmp(whichic, "IBV")) {
        if (libsfound) {
// The ibv_cardcheck() is worth considering for other libibverbs.so users, not just IBV
            if (ibv_cardcheck(handle[nlibs-1]) == 0) {
                extra_ibv_check = 1;
            }
        }
    }

    if (libsfound) {
        for (j = nlibs - 1; j >= 0; j--) {
            dlclose(handle[j]);
        }
    }

    if (!libsfound) {
        if (tooverbose) printf("dlopen test for %s could not "
              "open libs in list %s: %s\n",
              libs_var, getenv(libs_var), dlerror());
        return(0);
    }

// Extra tests hard-coded beyond the modules and loading of the libraries
// The ibv_cardcheck() is worth considering for other libibverbs.so users, not just IBV
    if (0 == strcmp(whichic, "IBV") && !extra_ibv_check) {
        if (tooverbose) printf("card check shows IBV "
            "not available.\n");
        return(0);
    }
    if (0 == strcmp(whichic, "PSM") ||
        0 == strcmp(whichic, "PSM2"))
    {
        if (!(
            LOOKUP_PCI("qlogic", 1) ||
            LOOKUP_PCI("Omni", 0)
            ))
        {
            if (tooverbose) printf("PSM card check not finding card.\n");
            return(0);
        }
    }

    // if all the checks passed
    return(1);
}

/*
 *  The input is MPI_IC_ORDER like "elan itapi gm tcp" that is
 *  constructed based on the users command line arguments and
 *  on MPI_IC_ORDER from a .conf file.
 */
int hpmp_ic_get_my_mask(void);
int
hpmp_ic_get_my_mask(void)

{
    char **ic_list;
    char *ic;
    char *ic_upper;
    int i, j, n;
    int mask;
    parse_helper_simple_list(&ic_list, getenv("MPI_IC_ORDER"));

// We'll check up through the first upper-case option and put those into a mask
// to indicate what was found

    i = 0;
    mask = 0;
    while ((ic = ic_list[i])) {
        ic_upper = strdup(ic);
        n = strlen(ic_upper);
        for (j=0; j<n; ++j) { ic_upper[j] = toupper(ic_upper[j]); }
        if (hasic(ic_upper)) {
            mask |= ic_string_to_bit(ic_upper);
        }
        free(ic_upper);

        if ('A' <= ic[0] && ic[0] <= 'Z') {
            break;
        }

        ++i;
    }

    free(ic_list);
    mask |= ic_string_to_bit("TCP"); // always allow this one

    return mask;
}

// Similar to the above, but here we input an available_mask and
// then look for the first MPI_IC_ORDER entry that is available
// and return a mask with just a single bit representing the selection
int hpmp_ic_get_my_bit(int available_mask);
int
hpmp_ic_get_my_bit(int available_mask)

{
    char **ic_list;
    char *ic;
    char *ic_upper;
    int i, j, n;
    int mask;
    int return_mask;
    parse_helper_simple_list(&ic_list, getenv("MPI_IC_ORDER"));

// We'll check up through the first upper-case option and put those into a mask
// to indicate what was found

    i = 0;
    mask = 0;
    return_mask = 0;
    while ((ic = ic_list[i])) {
        ic_upper = strdup(ic);
        n = strlen(ic_upper);
        for (j=0; j<n; ++j) { ic_upper[j] = toupper(ic_upper[j]); }
        mask = ic_string_to_bit(ic_upper);
        if (available_mask & mask) {
            return_mask = mask;
            break;
        }
        free(ic_upper);

        if ('A' <= ic[0] && ic[0] <= 'Z') {
            break;
        }

        ++i;
    }

    free(ic_list);

    return return_mask;
}

#if 0
int
hpmp_hasic_psm() {
#ifdef _WIN32
        return 0;
#else
        int has_qlogic, has_omnipath;
        has_qlogic  = hpmp_lspci("qlogic");
        has_omnipath  = hpmp_lspci("Omni"); // update this
        if (!has_qlogic && !has_omnipath) return 0;

        return hpmp_hasic("PSM");
#endif
}
#endif

static void
query_pmix_layout(pmix_layout_t *layout)
{
    opal_process_name_t wildcard_rank, proc_my_name;
    int u32, *u32ptr;
    uint16_t u16, *u16ptr;
    int ret;

    layout->has_rank_info = 0;
    layout->has_host_info = 0;
    layout->leader_ranks = NULL;

    u32ptr = &u32;
    u16ptr = &u16;

    proc_my_name = OPAL_PROC_MY_NAME;
    wildcard_rank = OPAL_PROC_MY_NAME;
    wildcard_rank.vpid = OPAL_VPID_WILDCARD;

// nranks, myrank
    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_JOB_SIZE,
                                   &wildcard_rank, &u32ptr, OPAL_UINT32);
    if (ret != PMIX_SUCCESS) { return; }
    layout->nranks = u32;

    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_RANK,
                                   &proc_my_name, &u32ptr, OPAL_UINT32);
    if (ret == PMIX_SUCCESS) {
        layout->myrank = u32;
    } else {
        layout->myrank = OPAL_PROC_MY_NAME.vpid;
    }

    layout->has_rank_info = 1;

// nhosts, myhost
    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_NUM_NODES,
                                   &wildcard_rank, &u32ptr, OPAL_UINT32);
    if (ret != PMIX_SUCCESS) { return; }
    layout->nhosts = u32;

    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_NODEID,
                                   &proc_my_name, &u32ptr, OPAL_UINT32);
    if (ret != PMIX_SUCCESS) { return; }
    layout->myhost = u32;

// nlranks, mylrank
    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_LOCAL_SIZE,
                                   &wildcard_rank, &u32ptr, OPAL_UINT32);
    if (ret != PMIX_SUCCESS) { return; }
    layout->nlranks = u32;

    OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_LOCAL_RANK,
                                   &proc_my_name, &u16ptr, PMIX_UINT16);
    layout->mylrank = u16;
    if (ret != PMIX_SUCCESS) { return; }

// Also for myrank == 0 we want to know a list of which ranks are host leaders
// eg which have LOCAL_RANK 0
// I'm reluctant to then skip over LOCAL_SIZE vpids to find the next leader though,
// unless there's some guarantee the IDs are contiguous

    if (layout->myrank == 0) {
        layout->leader_ranks = malloc(layout->nhosts * sizeof(int));
        opal_process_name_t peer_proc;
        peer_proc = OPAL_PROC_MY_NAME;
        int i, j;
        for(i=0,j=0;i<layout->nranks;++i) {
            peer_proc.vpid = i;

            OPAL_MODEX_RECV_VALUE_OPTIONAL(ret, PMIX_LOCAL_RANK,
                                           &peer_proc, &u16ptr, PMIX_UINT16);
            if (ret == PMIX_SUCCESS && u16 == 0 && j<layout->nhosts) {
                layout->leader_ranks[j++] = i;
            }
            if (ret != PMIX_SUCCESS) { return; }
        }
    }

    layout->has_host_info = 1;

#if 1
    printf("[%d] global(%d) %d/%d loc(%d) %d/%d host %d/%d\n", getpid(),
        layout->has_rank_info, layout->myrank, layout->nranks,
        layout->has_host_info, layout->mylrank, layout->nlranks,
        layout->myhost, layout->nhosts);
#endif
}

// gather 4b (int)
// depending on layout, either recv nranks items (1 from everybody) or nhosts
// items (1 from each host leader)
static int
my_pmix_gather(void *sbuf, void *rbuf, char *idstr, pmix_layout_t *layout)
{
    int ret, i;
    char *key = malloc(64 + strlen(idstr));
    opal_process_name_t proc_peer;
    int *u32ptr;
    pmix_info_t info[1];;

    if (!layout->has_rank_info) {
        return -1;
    }

    PMIX_INFO_CONSTRUCT(&info[0]);
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, NULL, PMIX_BOOL);

    sprintf(key, "platform-icsel-gather-%s", idstr);
    if (!layout->has_host_info) {
        // recv at 0 from every rank
        OPAL_MODEX_SEND_VALUE(ret, PMIX_GLOBAL, key, sbuf, OPAL_UINT32);
        if (ret != OPAL_SUCCESS) { return -1; }
        ret = PMIx_Commit();
        if (ret != PMIX_SUCCESS) { return -1; }
        ret = PMIx_Fence(NULL, 0, info, 1);
        if (ret != PMIX_SUCCESS) { return -1; }
        if (layout->myrank == 0) {
            for (i=0; i<layout->nranks; ++i) {
                proc_peer = OPAL_PROC_MY_NAME;
                proc_peer.vpid = i;
                u32ptr = (int*)((char*)rbuf + i * sizeof(int));
                OPAL_MODEX_RECV_VALUE(ret, key, &proc_peer, &u32ptr, OPAL_UINT32);
                if (ret != OPAL_SUCCESS) { return -1; }
            }
        }
    } else {
        // recv at 0 from every host leader
        if (layout->mylrank == 0) {
            OPAL_MODEX_SEND_VALUE(ret, PMIX_GLOBAL, key, sbuf, OPAL_UINT32);
        }
        if (ret != OPAL_SUCCESS) { return -1; }
        ret = PMIx_Commit();
        if (ret != PMIX_SUCCESS) { return -1; }
        ret = PMIx_Fence(NULL, 0, info, 1);
        if (ret != PMIX_SUCCESS) { return -1; }
        if (layout->myrank == 0) {
            for (i=0; i<layout->nhosts; ++i) {
                proc_peer = OPAL_PROC_MY_NAME;
                proc_peer.vpid = layout->leader_ranks[i];
                u32ptr = (int*)((char*)rbuf + i * sizeof(int));
                OPAL_MODEX_RECV_VALUE(ret, key, &proc_peer, &u32ptr, OPAL_UINT32);
                if (ret != OPAL_SUCCESS) { return -1; }
            }
        }
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    free(key);
    return 0;
}

// bcast 4b (int)
static int
my_pmix_bcast(void *buf, char *idstr, pmix_layout_t *layout)
{
    int ret;
    char *key = malloc(64 + strlen(idstr));
    opal_process_name_t proc_peer;
    int *u32ptr;
    pmix_info_t info[1];;

    if (!layout->has_rank_info) {
        return -1;
    }

    PMIX_INFO_CONSTRUCT(&info[0]);
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, NULL, PMIX_BOOL);

    sprintf(key, "platform-icsel-gather-%s", idstr);
    // send at 0 and all recv
    if (layout->myrank == 0) {
        OPAL_MODEX_SEND_VALUE(ret, PMIX_GLOBAL, key, buf, OPAL_UINT32);
        if (ret != OPAL_SUCCESS) { return -1; }
        ret = PMIx_Commit();
        if (ret != PMIX_SUCCESS) { return -1; }
    }
    ret = PMIx_Fence(NULL, 0, info, 1);
    if (ret != PMIX_SUCCESS) { return -1; }

    proc_peer = OPAL_PROC_MY_NAME;
    proc_peer.vpid = 0;
    u32ptr = (int*)buf;
    OPAL_MODEX_RECV_VALUE(ret, key, &proc_peer, &u32ptr, OPAL_UINT32);
    if (ret != OPAL_SUCCESS) { return -1; }

    PMIX_INFO_DESTRUCT(&info[0]);
    free(key);
    return 0;
}

static char *
bit_to_str(int bit) {
    if (bit == IC_MASK_PAMI) { return "PAMI"; }
    if (bit == IC_MASK_UCX) { return "UCX"; }
    if (bit == IC_MASK_MXM) { return "MXM"; }
    if (bit == IC_MASK_MXMC) { return "MXMC"; }
    if (bit == IC_MASK_IBV) { return "IBV"; }
    if (bit == IC_MASK_PSM) { return "PSM"; }
    if (bit == IC_MASK_PSM2) { return "PSM2"; }
    if (bit == IC_MASK_USNIC) { return "USNIC"; }
    if (bit == IC_MASK_TCP) { return "TCP"; }
    return "";
}

static void
mask_to_str(int mask, char *outstr) {
    int bit;
    outstr[0] = 0;
    for (bit=1; bit<=mask; bit*=2) {
        if (bit & mask) {
            if (outstr[0]) {
                strcat(outstr, ",");
            }
            strcat(outstr, bit_to_str(bit));
        }
    }
}

// If MPI_IC_ORDER is already set, assume smpi.conf is set up and everything is fine.
// If a value isn't provided, fill in simple defaults for all the IC-related settings.
static void pick_sane_default_env_settings(void);
static void
pick_sane_default_env_settings(void)
{
    // At first I was planning to early-return if there was a setting for MPI_IC_ORDER
    // but instead I'm just using setenv(,0) for all the settings.
    //if (getenv("MPI_IC_ORDER")) { return; }

// The format for *ICLIB* and *ICMOD* are things like "a || b || c d e || f g"
// For MPI_IC_ORDER its any of "a b c" or "a,b,c" or "a:b:c"
    setenv("MPI_IC_ORDER", "pami ucx mxm psm2 psm mxmc usnic ibv tcp", 0);

// Notes on whether to use libpami.so or mca_pml_pami.so etc:
//
// I like the cleanness of going straight to mca_pml_pami.so etc. There is an
// issue if this gets run at the orted level who doesn't necessarily have a good
// LD_LIBRARY_PATH in place when it starts. Its dlopen() calls wouldn't be
// searching MPI_ROOT/lib/ so it wouldn't find the libpami.so dependency of
// mca_pml_pami.so. And opening /full/path/to/mca_pml_pami.so doesn't help.
//
// And most of the other mca_stuff.so need MPI_ROOT/lib/libhwloc.so too, and would
// fail to dlopen for the same reason
//
// On the other hand /full/path/to/libpami.so can be dlopened.
//
// For now my solution is to expect this to be done at the ranks and hope that
// it's not necessary to push it earlier to the orted level

    setenv("MPI_ICLIB_PAMI", "mca_pml_pami.so", 0);
    setenv("MPI_ICMOD_PAMI", "ib_core", 0);

    setenv("MPI_ICLIB_UCX", "mca_pml_ucx.so", 0);
    setenv("MPI_ICMOD_UCX", "ib_core", 0);

    setenv("MPI_ICLIB_MXM", "mca_pml_yalla.so", 0);
    setenv("MPI_ICMOD_MXM", "ib_core", 0);

    setenv("MPI_ICLIB_MXMC", "mca_mtl_mxm.so", 0);
    setenv("MPI_ICMOD_MXMC", "ib_core", 0);

    setenv("MPI_ICLIB_IBV", "mca_btl_openib.so", 0);
    setenv("MPI_ICMOD_IBV", "ib_core", 0);

    setenv("MPI_ICLIB_PSM2", "mca_pml_psm2.so", 0);
    setenv("MPI_ICMOD_PSM2", "ib_qib || ib_ipath || hfi1", 0);

    setenv("MPI_ICLIB_PSM", "mca_pml_psm.so", 0);
    setenv("MPI_ICMOD_PSM", "ib_qib || ib_ipath || hfi1", 0);

    setenv("MPI_ICLIB_USNIC", "mca_btl_usnic.so", 0);
    setenv("MPI_ICMOD_USNIC", "ib_core", 0);

    setenv("MPI_ICLIB_TCP", "mca_btl_tcp.so", 0);
}

// Set OMPI vars based on the result contained in icbit
static void
setup_env_for_ic(int icbit) {
    if (icbit == IC_MASK_PAMI) {
        setenv("OMPI_MCA_pml", "pami", 1);
        setenv("OMPI_MCA_osc", "pami", 1);
        setenv("OMPI_MCA_btl", "self", 1);
    } else if (icbit == IC_MASK_UCX) {
        setenv("OMPI_MCA_pml", "ucx", 1);
        setenv("OMPI_MCA_osc", "ucx", 1);
    } else if (icbit == IC_MASK_MXM) {
        setenv("OMPI_MCA_pml", "yalla", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    } else if (icbit == IC_MASK_IBV) {
        setenv("OMPI_MCA_pml", "ob1", 1);
        setenv("OMPI_MCA_btl", "openib,self,vader", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    } else if (icbit == IC_MASK_PSM2) {
        setenv("OMPI_MCA_pml", "cm", 1);
        setenv("OMPI_MCA_mtl", "psm2", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    } else if (icbit == IC_MASK_PSM) {
        setenv("OMPI_MCA_pml", "cm", 1);
        setenv("OMPI_MCA_mtl", "psm", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    } else if (icbit == IC_MASK_USNIC) {
        setenv("OMPI_MCA_pml", "ob1", 1);
        setenv("OMPI_MCA_btl", "usnic,self,vader", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    } else if (icbit == IC_MASK_TCP) {
        setenv("OMPI_MCA_pml", "ob1", 1);
        setenv("OMPI_MCA_btl", "tcp,self,vader", 1);
        setenv("OMPI_MCA_osc", "^pami,ucx", 1);
    }
}
void opal_setic();
void
opal_setic()
{
    int too_early, verbose;
    static int didit = 0;
    char *p;
    int mask, bit, *masks;
    char maskstr[256];

    if (didit /*|| !opal_pmix.initialized || !opal_pmix.initialized()*/) {
        return;
    }
    if (getenv("MPI_IC_DONE")) {
        return;
    }
    query_pmix_layout(&pmix_layout);
    if (!pmix_layout.has_rank_info) {
        return;
    }

// The next early-return query I feel has a slight risk of not
// producing a globally consistent result, so I want to include it in the
// pmix gather/bcast rather than just "return" here.
    pick_sane_default_env_settings();
    too_early = 0;
    if (!hasic("TCP")) { too_early = 1; }

    mask = -123; // non-host leaders start here, get real data after the bcast
    if (!pmix_layout.has_host_info || pmix_layout.mylrank == 0) {
        mask = hpmp_ic_get_my_mask();
    }
    if (too_early) { mask = 0; } // the only way to get a 0 mask

    verbose = 0;
    p = getenv("MPI_IC_VERBOSE");
    if (p && *p && atoi(p)) { verbose = 1; }

    if (verbose) {
        mask_to_str(mask, maskstr);
        printf("[pid:%d %d/%d] local IC setting 0x%x [%s]\n",
            getpid(), pmix_layout.myrank, pmix_layout.nranks, mask,
            maskstr);
    }
    masks = malloc(pmix_layout.nranks * sizeof(int));
    my_pmix_gather(&mask, masks, "a", &pmix_layout);
    if (pmix_layout.myrank == 0) {
        int i, n = pmix_layout.nranks;
        if (pmix_layout.has_host_info) { n = pmix_layout.nhosts; };
        for (i=0; i<n; ++i) {
            mask = mask & masks[i]; // what bits are present at all ranks
        }
    }
    free(masks);
    my_pmix_bcast(&mask, "b", &pmix_layout);
    bit = hpmp_ic_get_my_bit(mask); // pick first MPI_IC_ORDER from the available mask
    if (verbose) {
        mask_to_str(bit, maskstr);
        printf("[pid:%d %d/%d] common mask %d reduced to [%d:%s]\n",
            getpid(), pmix_layout.myrank, pmix_layout.nranks,
            mask, bit, maskstr);
    }
    if (mask == 0) { too_early = 1; }

    if (!too_early) {
        setup_env_for_ic(bit);
        setenv("MPI_IC_DONE", "1", 1);
        didit = 1;
    }
    if (pmix_layout.leader_ranks) {
        free(pmix_layout.leader_ranks);
    }
}
