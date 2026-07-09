/*
 * repro_issue434.c — Regression guard for retired index_repository persistence.
 *
 * Historical #434 covered a bug where persistence=true did not create the first
 * .codebase-memory/graph.db.zst artifact. The public persistence flag is now
 * removed: a legacy client that still sends it must receive an explicit
 * unsupported-field error, and index_repository must not create repo-local
 * .codebase-memory artifacts as a side effect.
 */

#include "test_framework.h"
#include "test_helpers.h"
#include <foundation/compat.h>
#include <foundation/compat_fs.h>
#include <mcp/mcp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

TEST(repro_issue434_persistence_is_rejected) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_repro434_XXXXXX");
    if (!cbm_mkdtemp(tmpdir)) {
        FAIL("cbm_mkdtemp failed");
    }

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.c", tmpdir);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        th_rmtree(tmpdir);
        FAIL("fopen main.c failed");
    }
    fputs("int main(void) { return 0; }\n", fp);
    fclose(fp);

    char args[700];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"persistence\":true}", tmpdir);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        th_rmtree(tmpdir);
        FAIL("cbm_mcp_server_new failed");
    }

    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "unsupported field: persistence"));
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);
    cbm_mcp_server_free(srv);

    char art_dir[600];
    snprintf(art_dir, sizeof(art_dir), "%s/.codebase-memory", tmpdir);
    struct stat st;
    ASSERT_NEQ(stat(art_dir, &st), 0);

    th_rmtree(tmpdir);
    PASS();
}

SUITE(repro_issue434) {
    RUN_TEST(repro_issue434_persistence_is_rejected);
}
