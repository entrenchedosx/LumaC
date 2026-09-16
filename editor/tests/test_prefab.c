/* Phase 32 prefab depth tests: canonical determinism
 * (create -> bytes A; recreate same subtree -> same bytes modulo
 * the prefab UUID line), multi-instance isolation (10 instances:
 * disjoint handles, per-instance TRS independence), malformed
 * battery (truncated, bad magic, dup IDs, unknown root, second
 * prefab line, asset table, bad hex, unterminated object), nested
 * rejection (no prefab-in-prefab: a second `prefab` line fails),
 * 1k-instance stress (timed, non-blocking), duplicate-source
 * distinct identity (two copies of one prefab file -> two project
 * UUIDs after sidecar-distinct scan... covered at DB level;
 * here: two loads -> two registry assets). Headless. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <luma_editor/luma_editor.h>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

static int make_session(led_session **s, le_engine **e,
                        le_world **w) {
    le_engine_desc ed;
    le_world_desc wd;

    memset(&ed, 0, sizeof(ed));
    memset(&wd, 0, sizeof(wd));
    if (le_engine_create(&ed, e) != LE_SUCCESS) {
        return 0;
    }
    if (le_world_create(*e, &wd, w) != LE_SUCCESS) {
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_create(s) != LED_SUCCESS) {
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_attach(*s, *e, *w) != LED_SUCCESS) {
        led_session_destroy(*s);
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    return 1;
}

static void kill_session(led_session *s, le_engine *e,
                         le_world *w) {
    led_session_destroy(s);
    le_world_destroy(w);
    le_engine_destroy(e);
}

static void make_dir_one(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    long n = 0;
    char *buf = NULL;

    *out_size = 0;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *out_size = (size_t)n;
    return buf;
}

/* Strip identity lines (prefab/object/parent/prefab_root hexes
 * AND renderable/script asset hexes): two creates of one subtree
 * mint fresh UUIDs by design (distinct assets must never share
 * local IDs), so canonical determinism = identical line SHAPES +
 * identical field values in identical order. */
static int is_id_line(const char *p, size_t ll) {
    static const char *kKinds[] = {
        "prefab ", "object ", "parent ", "prefab_root ",
        "renderable ", "script ", "sprop asset ", "animator ",
        NULL,
    };
    int i;

    for (i = 0; kKinds[i] != NULL; i++) {
        size_t kl = strlen(kKinds[i]);

        if (ll > kl && strncmp(p, kKinds[i], kl) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Order-insensitive shape compare: records emit ID-sorted
 * (fresh random UUIDs per creation, so two creates may list
 * sibling objects in different relative order — same property
 * as scene capture). Split the stripped text into per-object
 * blocks at `end` lines, sort blocks, compare. */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a,
                  *(const char *const *)b);
}

static char *without_id_lines(const char *text);

static char *canonical_shape(const char *text) {
    char *stripped = without_id_lines(text);
    char *sorted = NULL;
    /* Block split (bounded: editor-scale payloads). */
    char **blocks = NULL;
    uint32_t nblocks = 0;
    uint32_t cblocks = 0;
    char *p;
    char *start;

    if (stripped == NULL) {
        return NULL;
    }
    /* Peel the header line as its own block (otherwise it glues
     * to whichever object the ID-sort emitted first, defeating
     * the sort). */
    {
        char *eol = strchr(stripped, '\n');

        if (eol != NULL) {
            size_t hl = (size_t)(eol + 1 - stripped);
            char *h = (char *)malloc(hl + 1u);

            if (h != NULL) {
                memcpy(h, stripped, hl);
                h[hl] = '\0';
                blocks = (char **)malloc(sizeof(*blocks));
                if (blocks != NULL) {
                    blocks[0] = h;
                    nblocks = 1;
                    cblocks = 1;
                } else {
                    free(h);
                }
            }
            p = (eol != NULL) ? eol + 1 : stripped;
            start = p;
        } else {
            p = stripped;
            start = stripped;
        }
    }
    if (blocks == NULL) {
        p = stripped;
        start = stripped;
    }
    while (*p != '\0') {
        if (strncmp(p, "end\n", 4) == 0) {
            size_t bl = (size_t)(p + 4 - start);
            char *b = (char *)malloc(bl + 1u);

            if (b == NULL) {
                break;
            }
            memcpy(b, start, bl);
            b[bl] = '\0';
            if (nblocks >= cblocks) {
                uint32_t grown =
                    (cblocks == 0) ? 16u : cblocks * 2u;
                char **fresh = (char **)realloc(
                    blocks, grown * sizeof(*fresh));

                if (fresh == NULL) {
                    free(b);
                    break;
                }
                blocks = fresh;
                cblocks = grown;
            }
            blocks[nblocks++] = b;
            p += 4;
            start = p;
            continue;
        }
        p++;
    }
    /* Trailing header text (magic + prefab_root) forms one last
     * block so header shapes compare too. */
    if (*start != '\0') {
        size_t bl = strlen(start);
        char *b = (char *)malloc(bl + 1u);

        if (b != NULL) {
            memcpy(b, start, bl + 1u);
            if (nblocks >= cblocks) {
                uint32_t grown =
                    (cblocks == 0) ? 16u : cblocks * 2u;
                char **fresh = (char **)realloc(
                    blocks, grown * sizeof(*fresh));

                if (fresh != NULL) {
                    blocks = fresh;
                    cblocks = grown;
                } else {
                    free(b);
                    b = NULL;
                }
            }
            if (b != NULL) {
                blocks[nblocks++] = b;
            }
        }
    }
    if (nblocks > 1) {
        qsort(blocks, nblocks, sizeof(*blocks), cmp_str);
    }
    {
        size_t total = 1;
        uint32_t i;

        for (i = 0; i < nblocks; i++) {
            total += strlen(blocks[i]);
        }
        sorted = (char *)malloc(total);
        if (sorted != NULL) {
            sorted[0] = '\0';
            for (i = 0; i < nblocks; i++) {
                strcat(sorted, blocks[i]);
                free(blocks[i]);
            }
        } else {
            for (i = 0; i < nblocks; i++) {
                free(blocks[i]);
            }
        }
    }
    free(blocks);
    free(stripped);
    return sorted;
}

static char *without_id_lines(const char *text) {
    size_t n = strlen(text);
    char *out = (char *)malloc(n + 1u);
    const char *p = text;
    char *q;

    if (out == NULL) {
        return NULL;
    }
    q = out;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t ll = (eol != NULL) ? (size_t)(eol - p)
                                  : strlen(p);

        /* Tolerate CRLF payloads (files written in text mode
         * before the binary-mode fix, or foreign tools). */
        while (ll > 0 && p[ll - 1] == '\r') {
            ll--;
        }
        if (!is_id_line(p, ll)) {
            memcpy(q, p, ll);
            q += ll;
            if (eol != NULL) {
                *q++ = '\n';
            }
        }
        p = (eol != NULL) ? eol + 1 : p + strlen(p);
    }
    *q = '\0';
    return out;
}

int main(void) {
    led_session *s = NULL;
    le_engine *e = NULL;
    le_world *w = NULL;
    char root[1024];
    char p[2048];
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMP");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = ".";
    }
    snprintf(root, sizeof(root), "%s/luma32_prefab", tmp);
    {
        static const char *kPrior[] = {
            "luma.project",
            "Assets/det.luprefab",
            "Assets/det.luprefab.luma",
            "Assets/multi.luprefab",
            "Assets/multi.luprefab.luma",
            "Assets/bad0.luprefab",
            "Assets/bad1.luprefab",
            "Assets/bad2.luprefab",
            "Assets/bad3.luprefab",
            "Assets/bad4.luprefab",
            "Assets/bad5.luprefab",
            "Assets/bad6.luprefab",
            "Assets/bad7.luprefab",
            NULL,
        };
        int i;

        for (i = 0; kPrior[i] != NULL; i++) {
            snprintf(p, sizeof(p), "%s/%s", root, kPrior[i]);
            remove(p);
        }
    }
    make_dir_one(root);
    snprintf(p, sizeof(p), "%s/Assets", root);
    make_dir_one(p);
    snprintf(p, sizeof(p), "%s/Scenes", root);
    make_dir_one(p);

    TEST_CHECK(make_session(&s, &e, &w), "make session");
    TEST_CHECK(led_project_create(root, "pf") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");

    /* Determinism: build subtree, create twice (two paths).
     * Fresh local UUIDs per creation BY DESIGN (two prefabs must
     * never share local IDs — same rule as scene IDs), so the
     * comparison strips identity lines and compares shapes. */
    {
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        float pp[3] = {1.0f, 0.0f, 0.0f};
        le_asset l1 = LE_ASSET_INVALID;
        le_asset l2 = LE_ASSET_INVALID;

        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS,
                   "det root");
        TEST_CHECK(le_object_set_name(w, &a, "det") ==
                       LE_SUCCESS,
                   "det name");
        TEST_CHECK(le_object_set_position(w, &a, pp) ==
                       LE_SUCCESS,
                   "det move");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS,
                   "det child");
        TEST_CHECK(le_object_set_parent(w, &b, &a) ==
                       LE_SUCCESS,
                   "det parent");
        TEST_CHECK(led_prefab_create(s, &a,
                                     "Assets/det.luprefab") ==
                       LED_SUCCESS,
                   "det create 1");
        TEST_CHECK(led_scene_new(s) == LED_SUCCESS,
                   "world reset");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS,
                   "det root 2");
        TEST_CHECK(le_object_set_name(w, &a, "det") ==
                       LE_SUCCESS,
                   "det name 2");
        TEST_CHECK(le_object_set_position(w, &a, pp) ==
                       LE_SUCCESS,
                   "det move 2");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS,
                   "det child 2");
        TEST_CHECK(le_object_set_parent(w, &b, &a) ==
                       LE_SUCCESS,
                   "det parent 2");
        TEST_CHECK(led_prefab_create(s, &a,
                                     "Assets/multi.luprefab") ==
                       LED_SUCCESS,
                   "det create 2");
        /* Load both (validates); compare canonical bytes. */
        TEST_CHECK(led_prefab_load(s, "Assets/det.luprefab",
                                   &l1) == LED_SUCCESS,
                   "load det 1");
        TEST_CHECK(led_prefab_load(s, "Assets/multi.luprefab",
                                   &l2) == LED_SUCCESS,
                   "load det 2");
        {
            size_t s1 = 0;
            size_t s2 = 0;
            char *t1 = NULL;
            char *t2 = NULL;
            char *c1 = NULL;
            char *c2 = NULL;
            char f1[2048];
            char f2[2048];

            snprintf(f1, sizeof(f1), "%s/Assets/det.luprefab",
                     root);
            snprintf(f2, sizeof(f2),
                     "%s/Assets/multi.luprefab", root);
            t1 = read_file(f1, &s1);
            t2 = read_file(f2, &s2);
            TEST_CHECK(t1 != NULL && t2 != NULL,
                       "read both payloads");
            if (t1 != NULL && t2 != NULL) {
                c1 = canonical_shape(t1);
                c2 = canonical_shape(t2);
                TEST_CHECK(c1 != NULL && c2 != NULL &&
                               strcmp(c1, c2) == 0,
                           "canonical shapes deterministic");
                free(c1);
                free(c2);
            }
            free(t1);
            free(t2);
        }
    }

    /* Multi-instance isolation: 10 instances, disjoint roots,
     * per-instance TRS independence (move one, others keep). */
    {
        le_asset prefab = LE_ASSET_INVALID;
        led_prefab_instance inst[10];
        uint32_t i;
        uint32_t j;

        memset(inst, 0, sizeof(inst));
        TEST_CHECK(led_prefab_load(s, "Assets/det.luprefab",
                                   &prefab) == LED_SUCCESS,
                   "load for multi");
        for (i = 0; i < 10; i++) {
            char msg[64];

            snprintf(msg, sizeof(msg), "instantiate %u", i);
            TEST_CHECK(led_prefab_instantiate(s, &prefab,
                                              &inst[i]) ==
                           LED_SUCCESS,
                       msg);
        }
        {
            int disjoint = 1;

            for (i = 0; i < 10 && disjoint; i++) {
                for (j = i + 1u; j < 10 && disjoint; j++) {
                    if (inst[i].root.index ==
                            inst[j].root.index &&
                        inst[i].root.generation ==
                            inst[j].root.generation) {
                        disjoint = 0;
                    }
                }
            }
            TEST_CHECK(disjoint, "10 roots disjoint");
        }
        /* Move instance 3's root; verify instance 5's root did
         * not follow (roots spawn at the baked {1,0,0}). */
        {
            float mv[3] = {99.0f, 0.0f, 0.0f};
            float got[3] = {0.0f, 0.0f, 0.0f};
            float got3[3] = {0.0f, 0.0f, 0.0f};

            TEST_CHECK(le_object_set_position(w, &inst[3].root,
                                              mv) == LE_SUCCESS,
                       "move instance 3");
            le_object_get_position(w, &inst[3].root, got3);
            le_object_get_position(w, &inst[5].root, got);
            TEST_CHECK(got3[0] == 99.0f, "instance 3 moved");
            TEST_CHECK(got[0] == 1.0f && got[1] == 0.0f &&
                           got[2] == 0.0f,
                       "instance 5 independent");
        }
        for (i = 0; i < 10; i++) {
            led_prefab_instance_free(&inst[i]);
        }
    }

    /* Malformed battery (each: PARSE or VALIDATION, world kept).
     * bad0: bad magic. bad1: truncated (no end/root).
     * bad2: duplicate object IDs. bad3: prefab_root unknown.
     * bad4: second prefab line (nested). bad5: asset table.
     * bad6: bad hex. bad7: missing prefab line. */
    {
        static const struct {
            const char *name;
            const char *body;
        } kBad[] = {
            {"Assets/bad0.luprefab", "NOPE 1\n"},
            {"Assets/bad1.luprefab",
             "LUMA_PREFAB 1\nprefab "
             "00000000000000010000000000000001\nobject "
             "00000000000000020000000000000002\nname \"x\"\n"},
            {"Assets/bad2.luprefab",
             "LUMA_PREFAB 1\nprefab "
             "00000000000000010000000000000001\nobject "
             "00000000000000020000000000000002\nend\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000020000000000000002\n"},
            {"Assets/bad3.luprefab",
             "LUMA_PREFAB 1\nprefab "
             "00000000000000010000000000000001\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000990000000000000099\n"},
            {"Assets/bad4.luprefab",
             "LUMA_PREFAB 1\nprefab "
             "00000000000000010000000000000001\nprefab "
             "00000000000000030000000000000003\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000020000000000000002\n"},
            {"Assets/bad5.luprefab",
             "LUMA_PREFAB 1\nprefab "
             "00000000000000010000000000000001\nasset mesh "
             "00000000000000020000000000000002 x\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000020000000000000002\n"},
            {"Assets/bad6.luprefab",
             "LUMA_PREFAB 1\nprefab zzz\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000020000000000000002\n"},
            {"Assets/bad7.luprefab",
             "LUMA_PREFAB 1\nobject "
             "00000000000000020000000000000002\nend\n"
             "prefab_root 00000000000000020000000000000002\n"},
            {NULL, NULL},
        };
        int bi;
        uint32_t before = le_world_get_object_count(w);

        for (bi = 0; kBad[bi].name != NULL; bi++) {
            char msg[64];
            char fp[2048];
            le_asset bad = LE_ASSET_INVALID;
            led_result rc;

            snprintf(fp, sizeof(fp), "%s/%s", root,
                     kBad[bi].name);
            write_file(fp, kBad[bi].body);
            rc = led_prefab_load(s, kBad[bi].name, &bad);
            snprintf(msg, sizeof(msg), "malformed %d rejected",
                     bi);
            TEST_CHECK(rc == LED_ERROR_PARSE ||
                           rc == LED_ERROR_VALIDATION,
                       msg);
            remove(fp);
        }
        TEST_CHECK(le_world_get_object_count(w) == before,
                   "world kept across malformed battery");
    }

    /* 1k-instance stress (timed; single plain object each). */
    {
        le_asset prefab = LE_ASSET_INVALID;
        clock_t t0;
        clock_t t1;
        double secs = 0.0;
        uint32_t i;
        uint32_t base = le_world_get_object_count(w);

        TEST_CHECK(led_prefab_load(s, "Assets/det.luprefab",
                                   &prefab) == LED_SUCCESS,
                   "load for stress");
        t0 = clock();
        for (i = 0; i < 1000; i++) {
            led_prefab_instance one;

            memset(&one, 0, sizeof(one));
            if (led_prefab_instantiate(s, &prefab, &one) !=
                LED_SUCCESS) {
                break;
            }
            led_prefab_instance_free(&one);
        }
        t1 = clock();
        secs = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
        printf("[INFO] 1k instantiates in %.3fs\n", secs);
        TEST_CHECK(i == 1000, "1k instances created");
        TEST_CHECK(
            le_world_get_object_count(w) == base + 1000 * 2,
            "1k instances live");
        TEST_CHECK(secs < 30.0, "1k instances non-blocking");
    }

    /* Duplicate loads: two loads of one file -> two registry
     * assets (registry identity is per-creation UUID; project
     * identity is the sidecar UUID — never conflated). */
    {
        le_asset p1 = LE_ASSET_INVALID;
        le_asset p2 = LE_ASSET_INVALID;
        le_asset_id i1;
        le_asset_id i2;

        memset(&i1, 0, sizeof(i1));
        memset(&i2, 0, sizeof(i2));
        TEST_CHECK(led_prefab_load(s, "Assets/det.luprefab",
                                   &p1) == LED_SUCCESS,
                   "load dup 1");
        TEST_CHECK(led_prefab_load(s, "Assets/det.luprefab",
                                   &p2) == LED_SUCCESS,
                   "load dup 2");
        le_asset_get_id(e, &p1, &i1);
        le_asset_get_id(e, &p2, &i2);
        TEST_CHECK(!le_asset_id_equal(&i1, &i2),
                   "registry loads distinct (project UUID same)");
    }

    kill_session(s, e, w);
    printf("prefab: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
