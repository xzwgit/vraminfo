/*
 * vraminfo - report NVIDIA GPU memory (VRAM) vendor, type and temperature on Linux.
 *
 * Two independent data sources:
 *
 *   1) Memory vendor / type  - read through the driver's official NVAPI bridge
 *      (libnvidia-api.so.1, present with any recent NVIDIA driver). Works
 *      unprivileged.  NvAPI_GPU_GetRamMaker / NvAPI_GPU_GetRamType.
 *
 *   2) Memory temperature    - read straight from the GPU's register aperture
 *      over MMIO (/dev/mem, read-only). Requires root. The register path is
 *      chosen from the memory type reported by NVAPI, so no per-model table is
 *      needed:
 *        - GDDR6X (Ampere / Ada) : BAR0+0xE2A8, temperature in bits [11:0],
 *                                  Celsius = field / 32.
 *        - GDDR7  (Blackwell)    : per-module DRAM sensors (DQR). Module p at
 *                                  BAR0+0x9024C0+p*0x4000, validity nibble in
 *                                  bits [27:24] of +0x10, temperature as an
 *                                  MR-code in bits [23:16].
 *        - other types           : no readable sensor, reported as unsupported.
 *
 * Register knowledge comes from the public reverse-engineering work of the
 * gddr6 project (https://github.com/olealgoritme/gddr6) and the wider modding
 * community; this is an independent implementation.
 *
 * Build:  make            (or: gcc -O2 -Wall -o vraminfo vraminfo.c -ldl)
 * Run:    sudo ./vraminfo [--json] [--per-module] [--watch]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_GPUS 32
#define DQR_MODULE0    0x009024C0u
#define DQR_VLD_OFF    0x10u          /* 0x9024D0 - 0x9024C0 */
#define DQR_STRIDE     0x00004000u
#define DQR_MAX_MODULES 16
#define ADA_MEMTEMP_OFF 0x0000E2A8u

/* ------------------------------------------------------------------ NVAPI */
typedef void *(*qif_t)(uint32_t);

static const char *maker_name(uint32_t v) {
    switch (v) {
    case 1:  return "Samsung";
    case 2:  return "Qimonda";
    case 3:  return "Elpida";
    case 4:  return "Etron";
    case 5:  return "Nanya";
    case 6:  return "Hynix";
    case 7:  return "Mosel";
    case 8:  return "Winbond";
    case 9:  return "ESMT";
    case 10: return "Micron";
    default: return NULL;
    }
}

static const char *type_name(uint32_t v) {
    switch (v) {
    case 8:  return "GDDR5";
    case 14: return "GDDR6";
    case 15: return "GDDR6X";
    case 16: return "GDDR7";
    default: return NULL;
    }
}

struct nvapi_gpu {
    int      index;
    int      have_bus;
    uint32_t bus;          /* PCI bus number, when the driver reports it */
    uint32_t maker, type;  /* raw enum values */
    int      have_ram;
};

/* ------------------------------------------------------- board (AIB) vendor */
/* The board partner lives in the PCI subsystem ID; readable on every card,
 * no root and no driver involvement. Names come from the system pci.ids
 * database when present, otherwise from this short table of common partners. */
static const struct { uint16_t id; const char *name; } board_vendors[] = {
    { 0x10de, "NVIDIA" },        { 0x1043, "ASUSTeK" },   { 0x1458, "Gigabyte" },
    { 0x1462, "MSI" },           { 0x196e, "PNY" },       { 0x19da, "ZOTAC" },
    { 0x3842, "EVGA" },          { 0x1b4c, "GALAX" },     { 0x1569, "Palit" },
    { 0x10b0, "Gainward" },      { 0x7377, "Colorful" },  { 0x1acc, "Inno3D" },
    { 0x1849, "ASRock" },        { 0x1b0a, "Pegatron" },  { 0x1028, "Dell" },
    { 0x103c, "HP" },            { 0x17aa, "Lenovo" },    { 0x1a03, "ASPEED" },
};

static const char *pci_ids_paths[] = {
    "/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids",
    "/usr/share/pci.ids", "/usr/local/share/pci.ids", NULL,
};

/* Look a 16-bit vendor ID up in pci.ids ("1043  ASUSTeK Computer Inc."). */
static const char *board_vendor_from_pci_ids(uint16_t id) {
    for (int i = 0; pci_ids_paths[i]; i++) {
        FILE *f = fopen(pci_ids_paths[i], "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            if (line[0] == '\t' || line[0] == ' ') continue;   /* device/class lines */
            unsigned vid;
            if (sscanf(line, "%4x", &vid) != 1) continue;
            if (vid != id) continue;
            /* name = rest of the line, trimmed */
            char *p = line + 4;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strlen(p);
            while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = 0;
            fclose(f);
            return n ? strdup(p) : NULL;   /* small leak per call is fine for a one-shot tool */
        }
        fclose(f);
    }
    return NULL;
}

/* Short name for the table: built-in list first, pci.ids as fallback. */
static const char *board_vendor_short(uint16_t id) {
    for (size_t i = 0; i < sizeof board_vendors / sizeof board_vendors[0]; i++)
        if (board_vendors[i].id == id) return board_vendors[i].name;
    return board_vendor_from_pci_ids(id);
}

/* Full official name for JSON: pci.ids first, built-in list as fallback. */
static const char *board_vendor_full(uint16_t id) {
    const char *n = board_vendor_from_pci_ids(id);
    if (n) return n;
    for (size_t i = 0; i < sizeof board_vendors / sizeof board_vendors[0]; i++)
        if (board_vendors[i].id == id) return board_vendors[i].name;
    return NULL;
}

struct nvapi {
    void *lib;
    int   ready;
    int   count;
    struct nvapi_gpu gpu[MAX_GPUS];
};

static int nvapi_open(struct nvapi *n) {
    memset(n, 0, sizeof *n);
    n->lib = dlopen("libnvidia-api.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!n->lib) n->lib = dlopen("libnvidia-api.so", RTLD_NOW | RTLD_GLOBAL);
    if (!n->lib) return -1;

    qif_t qif = (qif_t)dlsym(n->lib, "nvapi_QueryInterface");
    if (!qif) return -1;

    int (*init)(void) = (int (*)(void))qif(0x0150E828);          /* NvAPI_Initialize */
    int (*enumg)(void **, int *) = (int (*)(void **, int *))qif(0xE5AC921F);
    int (*getmaker)(void *, void *) = (int (*)(void *, void *))qif(0x42AEA16A);
    int (*gettype)(void *, void *) = (int (*)(void *, void *))qif(0x57F7CAAC);
    int (*getbus)(void *, void *) = (int (*)(void *, void *))qif(0x1BE0B8E5); /* NvAPI_GPU_GetBusId */
    if (!init || init() != 0 || !enumg) return -1;

    void *handles[MAX_GPUS];
    int n_gpu = 0;
    if (enumg(handles, &n_gpu) != 0 || n_gpu <= 0) return -1;
    if (n_gpu > MAX_GPUS) n_gpu = MAX_GPUS;
    n->count = n_gpu;

    for (int i = 0; i < n_gpu; i++) {
        struct nvapi_gpu *g = &n->gpu[i];
        unsigned char buf[64];
        g->index = i;
        if (getbus) {
            memset(buf, 0, sizeof buf);
            if (getbus(handles[i], buf) == 0) { g->bus = *(uint32_t *)buf; g->have_bus = 1; }
        }
        if (getmaker) {
            memset(buf, 0, sizeof buf);
            if (getmaker(handles[i], buf) == 0) { g->maker = *(uint32_t *)buf; g->have_ram = 1; }
        }
        if (gettype) {
            memset(buf, 0, sizeof buf);
            if (gettype(handles[i], buf) == 0) g->type = *(uint32_t *)buf;
        }
    }
    n->ready = 1;
    return 0;
}

/* ------------------------------------------------------- PCI enumeration */
struct pci_gpu {
    char     bdf[20];        /* 0000:02:00.0 */
    int      bus, dev, func;
    uint16_t device_id, sub_vendor, sub_device;
    uint64_t bar0;
    char     syspath[512];   /* /sys/bus/pci/devices/<bdf> */

    /* merged results */
    int      nvapi_index;    /* -1 when unmatched */
    uint32_t maker, type;
    int      have_ram;
    /* temperature */
    int      temp_c;         /* hotspot; -1000 = unavailable */
    int      temp_modules[DQR_MAX_MODULES];
    int      n_modules;
    char     temp_note[96];
};

static int read_text(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    /* strip trailing whitespace */
    while (r > 0 && isspace((unsigned char)buf[r - 1])) buf[--r] = 0;
    return 0;
}

static int cmp_pci(const void *a, const void *b) {
    const struct pci_gpu *x = a, *y = b;
    if (x->bus != y->bus) return x->bus - y->bus;
    if (x->dev != y->dev) return x->dev - y->dev;
    return x->func - y->func;
}

static int scan_gpus(struct pci_gpu *out) {
    const char *base = "/sys/bus/pci/devices";
    DIR *d = opendir(base);
    if (!d) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < MAX_GPUS) {
        if (e->d_name[0] == '.') continue;
        char path[600], buf[128];
        snprintf(path, sizeof path, "%s/%s/vendor", base, e->d_name);
        if (read_text(path, buf, sizeof buf) != 0) continue;
        if (strcasecmp(buf, "0x10de") != 0) continue;      /* NVIDIA */
        snprintf(path, sizeof path, "%s/%s/class", base, e->d_name);
        if (read_text(path, buf, sizeof buf) != 0) continue;
        /* 0x030000 VGA, 0x030200 3D controller */
        if (strncmp(buf, "0x03", 4) != 0) continue;

        if (strlen(e->d_name) >= 20) continue;
        struct pci_gpu *g = &out[n];
        memset(g, 0, sizeof *g);
        snprintf(g->bdf, sizeof g->bdf, "%s", e->d_name);
        snprintf(g->syspath, sizeof g->syspath, "%s/%s", base, e->d_name);
        g->nvapi_index = -1;
        g->temp_c = -1000;
        if (sscanf(e->d_name, "%x:%x:%x", &g->bus, &g->dev, &g->func) != 3) continue;

        snprintf(path, sizeof path, "%s/%s/device", base, e->d_name);
        if (read_text(path, buf, sizeof buf) == 0) g->device_id = (uint16_t)strtoul(buf, NULL, 0);
        snprintf(path, sizeof path, "%s/%s/subsystem_vendor", base, e->d_name);
        if (read_text(path, buf, sizeof buf) == 0) g->sub_vendor = (uint16_t)strtoul(buf, NULL, 0);
        snprintf(path, sizeof path, "%s/%s/subsystem_device", base, e->d_name);
        if (read_text(path, buf, sizeof buf) == 0) g->sub_device = (uint16_t)strtoul(buf, NULL, 0);

        /* BAR0 = first line of the resource file */
        snprintf(path, sizeof path, "%s/%s/resource", base, e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char line[256];
            ssize_t r = read(fd, line, sizeof line - 1);
            close(fd);
            if (r > 0) {
                line[r] = 0;
                unsigned long long start = 0;
                if (sscanf(line, "%llx", &start) == 1) g->bar0 = start;
            }
        }
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, cmp_pci);
    return n;
}

/* ------------------------------------------------------------- MMIO read */
static int mmio_read32(int fd, uint64_t phys, uint32_t *out) {
    long pg = sysconf(_SC_PAGE_SIZE);
    uint64_t base = phys & ~((uint64_t)pg - 1);
    void *map = mmap(NULL, (size_t)pg, PROT_READ, MAP_SHARED, fd, (off_t)base);
    if (map == MAP_FAILED) return -1;
    *out = *(volatile uint32_t *)((const char *)map + (phys - base));
    munmap(map, (size_t)pg);
    return 0;
}

/* GDDR temp MR-code -> Celsius (Blackwell DRAM sensor). */
static int decode_mrcode(uint32_t raw) {
    int code = (int)((raw >> 16) & 0xFF);
    if (code > 80) code = 80;
    return (code > 19) ? (code - 20) * 2 : -(40 - code * 2);
}

static int read_gddr7_modules(int fd, uint64_t bar0, struct pci_gpu *g) {
    int hot = -1000;
    for (int p = 0; p < DQR_MAX_MODULES; p++) {
        uint32_t off = DQR_MODULE0 + (uint32_t)p * DQR_STRIDE;
        uint32_t vld = 0, dq = 0;
        if (mmio_read32(fd, bar0 + off + DQR_VLD_OFF, &vld) != 0) return -1;
        if (mmio_read32(fd, bar0 + off, &dq) != 0) return -1;
        int all_valid = (((vld >> 24) & 0xF) == 0xF);
        int poison = ((dq & 0xFFFF0000u) == 0xBADF0000u);
        if (!all_valid || poison) continue;
        int c = decode_mrcode(dq);
        g->temp_modules[g->n_modules++] = c;
        if (c > hot) hot = c;
    }
    g->temp_c = hot;
    return (hot == -1000) ? -1 : 0;
}

static void read_temperatures(struct pci_gpu *gpus, int n) {
    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) {
        for (int i = 0; i < n; i++)
            snprintf(gpus[i].temp_note, sizeof gpus[i].temp_note,
                     "open(/dev/mem) failed: %s (run as root)", strerror(errno));
        return;
    }
    for (int i = 0; i < n; i++) {
        struct pci_gpu *g = &gpus[i];
        if (!g->have_ram || !g->type) {
            snprintf(g->temp_note, sizeof g->temp_note, "memory type unknown");
            continue;
        }
        if (g->type == 16) {                     /* GDDR7 -> Blackwell per-module DQR */
            if (g->bar0 == 0) {
                snprintf(g->temp_note, sizeof g->temp_note, "no BAR0");
                continue;
            }
            if (read_gddr7_modules(fd, g->bar0, g) != 0)
                snprintf(g->temp_note, sizeof g->temp_note,
                         "MMIO read failed (try kernel parameter iomem=relaxed)");
            continue;
        }
        if (g->type == 15) {                     /* GDDR6X -> Ampere/Ada register */
            uint32_t raw = 0;
            if (g->bar0 == 0) {
                snprintf(g->temp_note, sizeof g->temp_note, "no BAR0");
                continue;
            }
            if (mmio_read32(fd, g->bar0 + ADA_MEMTEMP_OFF, &raw) != 0) {
                snprintf(g->temp_note, sizeof g->temp_note,
                         "MMIO read failed (try kernel parameter iomem=relaxed)");
                continue;
            }
            g->temp_c = (int)((raw & 0x00000FFFu) / 0x20u);
            continue;
        }
        snprintf(g->temp_note, sizeof g->temp_note,
                 "no memory temperature sensor on this DRAM type");
    }
    close(fd);
}

/* ------------------------------------------------------------------ main */
static void print_json(struct pci_gpu *g, int n, int per_module) {
    printf("{\n  \"gpus\": [\n");
    for (int i = 0; i < n; i++) {
        struct pci_gpu *x = &g[i];
        const char *mk = x->have_ram ? maker_name(x->maker) : NULL;
        const char *tp = x->have_ram ? type_name(x->type) : NULL;
        printf("    {\n      \"bdf\": \"%s\",\n", x->bdf);
        printf("      \"device_id\": \"0x%04x\",\n", x->device_id);
        printf("      \"subsystem\": \"0x%04x:0x%04x\",\n", x->sub_vendor, x->sub_device);
        {
            const char *bv = board_vendor_full(x->sub_vendor);
            printf("      \"board_vendor\": %s%s%s,\n",
                   bv ? "\"" : "", bv ? bv : "null", bv ? "\"" : "");
        }
        if (mk || tp) {
            printf("      \"memory_maker\": %s%s%s,\n", mk ? "\"" : "", mk ? mk : "null", mk ? "\"" : "");
            printf("      \"memory_type\": %s%s%s,\n", tp ? "\"" : "", tp ? tp : "null", tp ? "\"" : "");
            printf("      \"memory_maker_id\": %u,\n      \"memory_type_id\": %u,\n", x->maker, x->type);
        } else {
            printf("      \"memory_maker\": null,\n      \"memory_type\": null,\n");
        }
        if (x->temp_c > -1000)
            printf("      \"memory_temp_c\": %d,\n", x->temp_c);
        else
            printf("      \"memory_temp_c\": null,\n");
        if (per_module && x->n_modules > 0) {
            printf("      \"memory_temp_modules_c\": [");
            for (int m = 0; m < x->n_modules; m++) printf("%s%d", m ? ", " : "", x->temp_modules[m]);
            printf("],\n");
        }
        printf("      \"note\": %s%s%s\n    }%s\n",
               x->temp_note[0] ? "\"" : "", x->temp_note[0] ? x->temp_note : "null",
               x->temp_note[0] ? "\"" : "", i + 1 < n ? "," : "");
    }
    printf("  ]\n}\n");
}

static void print_table(struct pci_gpu *g, int n, int per_module) {
    printf("%-14s %-8s %-11s %-14s %-9s %-16s %s\n",
           "PCI", "DEVICE", "BOARD", "MEMORY", "MAKER", "MEM TEMP", "NOTE");
    for (int i = 0; i < n; i++) {
        struct pci_gpu *x = &g[i];
        char mem[24] = "-", mk[16] = "-", temp[160] = "-", dev[16], board[20];
        const char *bv = board_vendor_short(x->sub_vendor);
        snprintf(dev, sizeof dev, "0x%04x", x->device_id);
        if (bv) snprintf(board, sizeof board, "%s", bv);
        else    snprintf(board, sizeof board, "0x%04x", x->sub_vendor);
        if (x->have_ram) {
            const char *t = type_name(x->type), *m = maker_name(x->maker);
            if (t) snprintf(mem, sizeof mem, "%s", t); else snprintf(mem, sizeof mem, "type#%u", x->type);
            if (m) snprintf(mk, sizeof mk, "%s", m); else snprintf(mk, sizeof mk, "maker#%u", x->maker);
        }
        if (x->temp_c > -1000) {
            if (per_module && x->n_modules > 0) {
                int p = snprintf(temp, sizeof temp, "%d C [", x->temp_c);
                for (int k = 0; k < x->n_modules && p < (int)sizeof temp - 8; k++)
                    p += snprintf(temp + p, sizeof temp - p, "%s%d", k ? " " : "", x->temp_modules[k]);
                snprintf(temp + p, sizeof temp - p, "]");
            } else {
                snprintf(temp, sizeof temp, "%d C", x->temp_c);
            }
        }
        printf("%-14s %-8s %-11s %-14s %-9s %-16s %s\n",
               x->bdf + (strlen(x->bdf) > 4 ? 5 : 0), dev, board, mem, mk, temp, x->temp_note);
    }
}

int main(int argc, char **argv) {
    int json = 0, per_module = 0, watch = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--per-module")) per_module = 1;
        else if (!strcmp(argv[i], "--watch")) watch = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: vraminfo [--json] [--per-module] [--watch]\n\n");
            printf("  Reads NVIDIA VRAM vendor/type (NVAPI) and VRAM temperature (MMIO).\n");
            printf("  Temperature reading needs root and, on some kernels, iomem=relaxed.\n");
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    struct nvapi nv;
    int have_nvapi = (nvapi_open(&nv) == 0);

    do {
        struct pci_gpu gpus[MAX_GPUS];
        int n = scan_gpus(gpus);
        if (n < 0) { fprintf(stderr, "cannot enumerate PCI devices\n"); return 1; }
        if (n == 0) { fprintf(stderr, "no NVIDIA GPU found\n"); return 1; }

        if (have_nvapi) {
            /* Preferred: match NVAPI device -> PCI device by bus number.
             * Both enumerations are PCI-ordered, so fall back to plain index
             * matching when the driver does not report bus numbers. */
            int matched_all = (nv.count == n);
            for (int i = 0; i < n && matched_all; i++) {
                int found = -1;
                for (int k = 0; k < nv.count; k++)
                    if (nv.gpu[k].have_bus && (int)nv.gpu[k].bus == gpus[i].bus) { found = k; break; }
                gpus[i].nvapi_index = found;
                if (found < 0) matched_all = 0;
            }
            if (!matched_all)
                for (int i = 0; i < n; i++)
                    gpus[i].nvapi_index = (i < nv.count) ? i : -1;

            for (int i = 0; i < n; i++) {
                int k = gpus[i].nvapi_index;
                if (k < 0 || k >= nv.count) continue;
                gpus[i].have_ram = nv.gpu[k].have_ram;
                gpus[i].maker = nv.gpu[k].maker;
                gpus[i].type = nv.gpu[k].type;
            }
        }

        read_temperatures(gpus, n);

        if (json) print_json(gpus, n, per_module);
        else {
            if (!have_nvapi)
                fprintf(stderr,
                        "warning: libnvidia-api.so.1 unavailable - memory maker/type unavailable\n");
            print_table(gpus, n, per_module);
        }
        if (!watch) break;
        fflush(stdout);
        sleep(2);
        if (!json) printf("\n");
    } while (watch);

    return 0;
}
