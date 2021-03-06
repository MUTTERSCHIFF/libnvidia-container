/*
 * Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
 */

#include <gnu/lib-names.h>

#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <elfdefinitions.h>
#include <pci-enum.h>
#include <nvidia-modprobe-utils.h>

#include "nvc_internal.h"

#include "driver.h"
#include "debug.h"
#include "error.h"
#include "options.h"
#include "utils.h"
#include "xfuncs.h"

static void load_kernel_modules(void);
static int  copy_config(struct error *, struct nvc_context *, const struct nvc_config *);

const char interpreter[] __attribute__((section(".interp"))) = LIB_DIR "/" LD_SO;

const struct __attribute__((__packed__)) {
        Elf_Note hdr;
        uint32_t desc[5];
} abitag __attribute__((section (".note.ABI-tag"))) = {
        {0x04, 0x10, 0x01},
        {0x554e47, 0x0, 0x3, 0xa, 0x0}, /* GNU Linux 3.10.0 */
};

static const struct nvc_version version = {
        NVC_MAJOR,
        NVC_MINOR,
        NVC_PATCH,
        NVC_VERSION,
};

void
nvc_entrypoint(void)
{
        printf("version: %s\n", NVC_VERSION);
        printf("build date: %s\n", BUILD_DATE);
        printf("build revision: %s\n", BUILD_REVISION);
        printf("build flags: %s\n", BUILD_FLAGS);
        exit(EXIT_SUCCESS);
}

const struct nvc_version *
nvc_version(void)
{
        return (&version);
}

struct nvc_config *
nvc_config_new(void)
{
        struct nvc_config *cfg;

        if ((cfg = calloc(1, sizeof(*cfg))) == NULL)
                return (NULL);
        cfg->uid = (uid_t)-1;
        cfg->gid = (gid_t)-1;
        return (cfg);
}

void
nvc_config_free(struct nvc_config *cfg)
{
        if (cfg == NULL)
                return;
        free(cfg);
}

struct nvc_context *
nvc_context_new(void)
{
        struct nvc_context *ctx;

        if ((ctx = calloc(1, sizeof(*ctx))) == NULL)
                return (NULL);
        return (ctx);
}

void
nvc_context_free(struct nvc_context *ctx)
{
        if (ctx == NULL)
                return;
        error_reset(&ctx->err);
        free(ctx);
}

static void
load_kernel_modules(void)
{
        struct pci_id_match devs = {
                0x10de,        /* vendor (NVIDIA) */
                PCI_MATCH_ANY, /* device */
                PCI_MATCH_ANY, /* subvendor */
                PCI_MATCH_ANY, /* subdevice */
                0x0300,        /* class (display) */
                0xff00,        /* class mask (any subclass) */
                0,             /* match count */
        };

        if (pci_enum_match_id(&devs) != 0 || devs.num_matches == 0)
                log_warn("failed to detect NVIDIA devices");

        log_info("loading kernel module nvidia");
        if (nvidia_modprobe(0, -1) == 0)
                log_err("could not load kernel module nvidia");
        else {
                if (nvidia_mknod(NV_CTL_DEVICE_MINOR, -1) == 0)
                        log_err("could not create kernel module device node");
                for (int i = 0; i < (int)devs.num_matches; ++i) {
                        if (nvidia_mknod(i, -1) == 0)
                                log_err("could not create kernel module device node");
                }
        }

        log_info("loading kernel module nvidia_uvm");
        if (nvidia_uvm_modprobe() == 0)
                log_err("could not load kernel module nvidia_uvm");
        else {
                if (nvidia_uvm_mknod(0) == 0)
                        log_err("could not create kernel module device node");
        }

        log_info("loading kernel module nvidia_modeset");
        if (nvidia_modeset_modprobe() == 0)
                log_err("could not load kernel module nvidia_modeset");
        else {
                if (nvidia_modeset_mknod() == 0)
                        log_err("could not create kernel module device node");
        }
}

static int
copy_config(struct error *err, struct nvc_context *ctx, const struct nvc_config *cfg)
{
        const char *ldcache;
        unsigned long uid, gid;

        ldcache = (cfg->ldcache != NULL) ? cfg->ldcache : LDCACHE_PATH;
        if ((ctx->cfg.ldcache = xrealpath(err, ldcache, NULL)) == NULL)
                return (-1);

        if (cfg->uid != (uid_t)-1)
                ctx->cfg.uid = cfg->uid;
        else {
                if (file_read_ulong(err, PROC_OVERFLOW_UID, &uid) < 0)
                        return (-1);
                ctx->cfg.uid = (uid_t)uid;
        }
        if (cfg->gid != (gid_t)-1)
                ctx->cfg.gid = cfg->gid;
        else {
                if (file_read_ulong(err, PROC_OVERFLOW_GID, &gid) < 0)
                        return (-1);
                ctx->cfg.gid = (gid_t)gid;
        }

        log_infof("using ldcache %s", ctx->cfg.ldcache);
        log_infof("using unprivileged user %lu:%lu", (unsigned long)ctx->cfg.uid, (unsigned long)ctx->cfg.gid);
        return (0);
}

int
nvc_init(struct nvc_context *ctx, const struct nvc_config *cfg, const char *opts)
{
        int32_t flags;
        char path[PATH_MAX];

        if (ctx == NULL)
                return (-1);
        if (ctx->initialized)
                return (0);
        if (cfg == NULL)
                cfg = &(struct nvc_config){NULL, (uid_t)-1, (gid_t)-1};
        if (validate_args(ctx, !strempty(cfg->ldcache)) < 0)
                return (-1);
        if (opts == NULL)
                opts = default_library_opts;
        if ((flags = options_parse(&ctx->err, opts, library_opts, nitems(library_opts))) < 0)
                return (-1);

        log_open(secure_getenv("NVC_DEBUG_FILE"));
        log_infof("initializing library context (version=%s, build=%s)", NVC_VERSION, BUILD_REVISION);

        if (flags & OPT_LOAD_KMODS)
                load_kernel_modules();

        memset(&ctx->cfg, 0, sizeof(ctx->cfg));
        ctx->mnt_ns = -1;

        if (copy_config(&ctx->err, ctx, cfg) < 0)
                goto fail;
        if (xsnprintf(&ctx->err, path, sizeof(path), PROC_NS_PATH(PROC_SELF), "mnt") < 0)
                goto fail;
        if ((ctx->mnt_ns = xopen(&ctx->err, path, O_RDONLY|O_CLOEXEC)) < 0)
                goto fail;
        if (driver_init(&ctx->drv, &ctx->err, ctx->cfg.uid, ctx->cfg.gid) < 0)
                goto fail;

        ctx->initialized = true;
        return (0);

 fail:
        free(ctx->cfg.ldcache);
        xclose(ctx->mnt_ns);
        return (-1);
}

int
nvc_shutdown(struct nvc_context *ctx)
{
        if (ctx == NULL)
                return (-1);
        if (!ctx->initialized)
                return (0);

        log_info("shutting down library context");
        if (driver_shutdown(&ctx->drv) < 0)
                return (-1);
        free(ctx->cfg.ldcache);
        xclose(ctx->mnt_ns);

        memset(&ctx->cfg, 0, sizeof(ctx->cfg));
        ctx->mnt_ns = -1;

        log_close();
        ctx->initialized = false;
        return (0);
}

const char *
nvc_error(struct nvc_context *ctx)
{
        if (ctx == NULL)
                return (NULL);
        if (ctx->err.code != 0 && ctx->err.msg == NULL)
                return ("unknown error");
        return (ctx->err.msg);
}
