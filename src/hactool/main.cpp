#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <napi.h>
#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include "types.h"
#include "utils.h"
#include "settings.h"
#include "pki.h"
#include "nca.h"
#include "xci.h"
#include "nax0.h"
#include "extkeys.h"
#include "packages.h"
#include "nso.h"
#include "save.h"
#include "output.h"

#pragma message("Importing hactool")

static const char *prog_name = "hactool";

// Add the job type
char *finish(hactool_ctx_t *tool_ctx) {
    char *string = NULL;

    // Add the job type to the JSON object.
    cJSON_AddItemToObject(tool_ctx->output, "job", cJSON_CreateString(tool_ctx->action & ACTION_EXTRACT ? "extraction" : "information"));

    // Compile the JSON string.
    string = tool_ctx->minified ? cJSON_PrintUnformatted(tool_ctx->output) : cJSON_Print(tool_ctx->output);

    if (string == NULL) {
        return "{\"error\": true, \"errorMessage\": \"Failed to encode the JSON string when finishing.\"}";
    }

    return string;
}

// Add the error message then call the finish function.
char *stop(hactool_ctx_t *tool_ctx, char *text) {
    cJSON_ReplaceItemInObjectCaseSensitive(tool_ctx->output, "error", cJSON_CreateTrue());
    cJSON_ReplaceItemInObjectCaseSensitive(tool_ctx->output, "errorMessage", cJSON_CreateString(text));

    return finish(tool_ctx);
}

// Run the program.
char *start(int argc, char **argv, Napi::Env Env) {
    // Reset getopt because it uses global variables that will prevent subsequent calls to start() from properly parsing the arguments.
    optind = 1;

    hactool_ctx_t tool_ctx;
    // Context for base NCA, if used.
    hactool_ctx_t base_ctx;
    nca_ctx_t nca_ctx;
    char input_name[0x200];

    filepath_t keypath;
    filepath_init(&keypath);

    prog_name = (argc < 1) ? "hactool" : argv[0];

    nca_init(&nca_ctx);
    memset(&tool_ctx, 0, sizeof(tool_ctx));
    memset(&base_ctx, 0, sizeof(base_ctx));
    memset(input_name, 0, sizeof(input_name));

    nca_ctx.tool_ctx = &tool_ctx;
    nca_ctx.is_cli_target = true;

    nca_ctx.tool_ctx->file_type = FILETYPE_NCA;
    nca_ctx.tool_ctx->output = cJSON_CreateObject();
    nca_ctx.tool_ctx->warnings = cJSON_CreateArray();
    nca_ctx.tool_ctx->log = cJSON_CreateArray();

    base_ctx.file_type = FILETYPE_NCA;

    nca_ctx.tool_ctx->action = ACTION_INFO;
    pki_initialize_keyset(&tool_ctx.settings.keyset, KEYSET_RETAIL);

    // Attach the log and warnings arrays to the JSON object.
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "warnings", nca_ctx.tool_ctx->warnings);
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "log", nca_ctx.tool_ctx->log);

    // Initialize the skeleton of the JSON object.
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "error", cJSON_CreateFalse());
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "errorMessage", cJSON_CreateString(""));
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "keyset", cJSON_CreateString(tool_ctx.action & ACTION_DEV ? "dev.keys" : "prod.keys"));

    // Add an array of the arguments that were sent to this tool.
    cJSON *arguments = cJSON_CreateArray();
    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "arguments", arguments);

    for (int i = 0; i < argc; i++) {
        cJSON_AddItemToArray(arguments, cJSON_CreateString(argv[i]));
    }

    while (1) {
        int option_index;
        int c;
        static struct option long_options[] = {
            {"extract", 0, NULL, 100},
            {"info", 0, NULL, 101},
            {"file", 1, NULL, 102},
            {"dev", 0, NULL, 103},
            {"verify", 0, NULL, 104},
            {"raw", 0, NULL, 105},
            {"intype", 1, NULL, 106},
            {"minified", 0, NULL, 107},
            {"keyset", 1, NULL, 108},
            {"section0", 1, NULL, 0},
            {"section1", 1, NULL, 1},
            {"section2", 1, NULL, 2},
            {"section3", 1, NULL, 3},
            {"section0dir", 1, NULL, 4},
            {"section1dir", 1, NULL, 5},
            {"section2dir", 1, NULL, 6},
            {"section3dir", 1, NULL, 7},
            {"exefs", 1, NULL, 8},
            {"romfs", 1, NULL, 9},
            {"exefsdir", 1, NULL, 10},
            {"romfsdir", 1, NULL, 11},
            {"titlekey", 1, NULL, 12},
            {"contentkey", 1, NULL, 13},
            {"listromfs", 0, NULL, 14},
            {"baseromfs", 1, NULL, 15},
            {"basenca", 1, NULL, 16},
            {"outdir", 1, NULL, 17},
            {"plaintext", 1, NULL, 18},
            {"header", 1, NULL, 19},
            {"pfs0dir", 1, NULL, 20},
            {"hfs0dir", 1, NULL, 21},
            {"rootdir", 1, NULL, 22},
            {"updatedir", 1, NULL, 23},
            {"normaldir", 1, NULL, 24},
            {"securedir", 1, NULL, 25},
            {"logodir", 1, NULL, 26},
            {"package1dir", 1, NULL, 27},
            {"package2dir", 1, NULL, 28},
            {"ini1dir", 1, NULL, 29},
            {"extractini1", 0, NULL, 30},
            {"basefake", 0, NULL, 31},
            {"onlyupdated", 0, NULL, 32},
            {"sdseed", 1, NULL, 33},
            {"sdpath", 1, NULL, 34},
            {"sbk", 1, NULL, 35},
            {"tseckey", 1, NULL, 36},
            {"json", 1, NULL, 37},
            {"saveini1json", 0, NULL, 38},
            {"uncompressed", 1, NULL, 39},
            {"disablekeywarns", 0, NULL, 40},
            {"listfiles", 0, NULL, 41},
            {"xcontenttype", 1, NULL, 42},
            {"appendsectypes", 0, NULL, 43},
            {"suppresskeys", 0, NULL, 44},
            {NULL, 0, NULL, 0},
        };

        c = getopt_long(argc, argv, "dryxtmf:ik:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 101:
                nca_ctx.tool_ctx->action |= ACTION_INFO;

                addLogLine(nca_ctx.tool_ctx, "--info Setting the action to information.");
                break;
            case 100:
                nca_ctx.tool_ctx->action |= ACTION_EXTRACT;

                addLogLine(nca_ctx.tool_ctx, "--extract Setting the action to extraction.");
                break;
            case 102:
                nca_ctx.tool_ctx->action |= ACTION_EXTRACT;
                tool_ctx.settings.single_file.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.single_file.path, optarg);

                addLogLine(nca_ctx.tool_ctx, fmt::format("--file Extract only the {} file.", optarg).data());
                break;
            case 104:
                nca_ctx.tool_ctx->action |= ACTION_VERIFY;
                break;
            case 105:
                nca_ctx.tool_ctx->action |= ACTION_RAW;
                break;
            case 103:
                pki_initialize_keyset(&tool_ctx.settings.keyset, KEYSET_DEV);
                nca_ctx.tool_ctx->action |= ACTION_DEV;
                break;
            case 108:
                filepath_set(&keypath, optarg);
                break;
            case 107:
                addLogLine(nca_ctx.tool_ctx, "--minified The JSON output will be minified.");
                nca_ctx.tool_ctx->minified = true;
                break;
            case 106:
                addLogLine(nca_ctx.tool_ctx, fmt::format("--intype Setting the input file type to {}.", optarg).data());

                if (!strcmp(optarg, "nca")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NCA;
                } else if (!strcmp(optarg, "pfs0") || !strcmp(optarg, "exefs")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_PFS0;
                } else if (!strcmp(optarg, "romfs")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_ROMFS;
                } else if (!strcmp(optarg, "nca0_romfs") || !strcmp(optarg, "nca0romfs") || !strcmp(optarg, "betaromfs") || !strcmp(optarg, "beta_romfs")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NCA0_ROMFS;
                } else if (!strcmp(optarg, "hfs0")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_HFS0;
                } else if (!strcmp(optarg, "xci") || !strcmp(optarg, "gamecard") || !strcmp(optarg, "gc")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_XCI;
                } else if (!strcmp(optarg, "npdm") || !strcmp(optarg, "meta")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NPDM;
                } else if (!strcmp(optarg, "package1") || !strcmp(optarg, "pk11")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_PACKAGE1;
                } else if (!strcmp(optarg, "package2") || !strcmp(optarg, "pk21")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_PACKAGE2;
                } else if (!strcmp(optarg, "ini1")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_INI1;
                } else if (!strcmp(optarg, "kip1") || !strcmp(optarg, "kip")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_KIP1;
                } else if (!strcmp(optarg, "nso0") || !strcmp(optarg, "nso")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NSO0;
                } else if (!strcmp(optarg, "nax0") || !strcmp(optarg, "nax")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NAX0;
                } else if (!strcmp(optarg, "keygen") || !strcmp(optarg, "keys") || !strcmp(optarg, "boot0") || !strcmp(optarg, "boot")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_BOOT0;
                } else if (!strcmp(optarg, "save")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_SAVE;
                }
                break;
            case 0: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[0], optarg); break;
            case 1: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[1], optarg); break;
            case 2: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[2], optarg); break;
            case 3: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[3], optarg); break;
            case 4: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[0], optarg); break;
            case 5: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[1], optarg); break;
            case 6: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[2], optarg); break;
            case 7: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[3], optarg); break;
            case 8:
                nca_ctx.tool_ctx->settings.exefs_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.exefs_path.path, optarg);
                break;
            case 9:
                nca_ctx.tool_ctx->settings.romfs_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.romfs_path.path, optarg);
                break;
            case 10:
                nca_ctx.tool_ctx->settings.exefs_dir_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.exefs_dir_path.path, optarg);
                break;
            case 11:
                nca_ctx.tool_ctx->settings.romfs_dir_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.romfs_dir_path.path, optarg);
                break;
            case 12:
                parse_hex_key(nca_ctx.tool_ctx->settings.cli_titlekey, optarg, 16);
                nca_ctx.tool_ctx->settings.has_cli_titlekey = 1;
                break;
            case 13:
                parse_hex_key(nca_ctx.tool_ctx->settings.cli_contentkey, optarg, 16);
                nca_ctx.tool_ctx->settings.has_cli_contentkey = 1;
                break;
            case 14:
                nca_ctx.tool_ctx->action |= ACTION_LISTROMFS;
                break;
            case 15:
                if (nca_ctx.tool_ctx->base_file != NULL) {
                    return stop(nca_ctx.tool_ctx, "[baseromfs] The base file is not null.");
                }

                if ((nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")) == NULL) {
                    return stop(nca_ctx.tool_ctx, fmt::format("[baseromfs] unable to open {}: {}", optarg, strerror(errno)).data());
                }

                nca_ctx.tool_ctx->base_file_type = BASEFILE_ROMFS;
                break;
            case 16:
                if (nca_ctx.tool_ctx->base_file != NULL) {
                    return stop(nca_ctx.tool_ctx, "[basenca] The base file is not null.");
                }

                if ((nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")) == NULL) {
                    return stop(nca_ctx.tool_ctx, fmt::format("[basenca] unable to open {}: {}", optarg, strerror(errno)).data());
                }

                nca_ctx.tool_ctx->base_file_type = BASEFILE_NCA;
                nca_ctx.tool_ctx->base_nca_ctx = (nca_ctx_t*) malloc(sizeof(*nca_ctx.tool_ctx->base_nca_ctx));

                if (nca_ctx.tool_ctx->base_nca_ctx == NULL) {
                    return stop(nca_ctx.tool_ctx, "Failed to allocate base NCA context!");
                }

                nca_init(nca_ctx.tool_ctx->base_nca_ctx);
                base_ctx.file = nca_ctx.tool_ctx->base_file;
                nca_ctx.tool_ctx->base_nca_ctx->file = base_ctx.file;
                nca_ctx.tool_ctx->base_nca_ctx->is_cli_target = false;
                break;
            case 17:
                addLogLine(nca_ctx.tool_ctx, fmt::format("--outdir The file(s) will be extracted to {}.", optarg).data());

                tool_ctx.settings.out_dir_path.enabled = 1;
                filepath_set(&tool_ctx.settings.out_dir_path.path, optarg);
                break;
            case 18:
                filepath_set(&nca_ctx.tool_ctx->settings.plaintext_path, optarg);
                break;
            case 19:
                filepath_set(&nca_ctx.tool_ctx->settings.header_path, optarg);
                break;
            case 20:
                filepath_set(&tool_ctx.settings.pfs0_dir_path, optarg);
                break;
            case 21:
                filepath_set(&tool_ctx.settings.hfs0_dir_path, optarg);
                break;
            case 22:
                filepath_set(&tool_ctx.settings.rootpt_dir_path, optarg);
                break;
            case 23:
                filepath_set(&tool_ctx.settings.update_dir_path, optarg);
                break;
            case 24:
                filepath_set(&tool_ctx.settings.normal_dir_path, optarg);
                break;
            case 25:
                filepath_set(&tool_ctx.settings.secure_dir_path, optarg);
                break;
            case 26:
                filepath_set(&tool_ctx.settings.logo_dir_path, optarg);
                break;
            case 27:
                filepath_set(&tool_ctx.settings.pk11_dir_path, optarg);
                break;
            case 28:
                filepath_set(&tool_ctx.settings.pk21_dir_path, optarg);
                break;
            case 29:
                filepath_set(&tool_ctx.settings.ini1_dir_path, optarg);
                break;
            case 30:
                tool_ctx.action |= ACTION_EXTRACTINI1;
                break;
            case 31:
                if (nca_ctx.tool_ctx->base_file != NULL) {
                    return stop(nca_ctx.tool_ctx, "[basefake] The base file is not null.");
                }

                nca_ctx.tool_ctx->base_file_type = BASEFILE_FAKE;
                nca_ctx.tool_ctx->base_file++; /* Guarantees base_file != NULL. I'm so sorry. */
                break;
            case 32:
                tool_ctx.action |= ACTION_ONLYUPDATEDROMFS;
                break;
            case 33:
                parse_hex_key(nca_ctx.tool_ctx->settings.sdseed, optarg, 16);
                nca_ctx.tool_ctx->settings.has_sdseed = 1;

                for (unsigned int key = 0; key < 2; key++) {
                    for (unsigned int i = 0; i < 0x20; i++) {
                        tool_ctx.settings.keyset.sd_card_key_sources[key][i] ^= tool_ctx.settings.sdseed[i & 0xF];
                    }
                }

                pki_derive_keys(&tool_ctx.settings.keyset);
                break;
            case 34:
                filepath_set(&tool_ctx.settings.nax0_sd_path, optarg);
                break;
            case 35:
                parse_hex_key(nca_ctx.tool_ctx->settings.keygen_sbk, optarg, 16);
                break;
            case 36:
                parse_hex_key(nca_ctx.tool_ctx->settings.keygen_tsec, optarg, 16);
                break;
            case 37:
                filepath_set(&tool_ctx.settings.npdm_json_path, optarg);
                break;
            case 38:
                tool_ctx.action |= ACTION_SAVEINIJSON;
                break;
            case 39:
                filepath_set(&nca_ctx.tool_ctx->settings.uncompressed_path, optarg);
                break;
            case 40:
                addLogLine(nca_ctx.tool_ctx, "--disablekeywarns Supressing the missing key warnings.");

                nca_ctx.tool_ctx->settings.skip_key_warnings = 1;
                break;
            case 41:
                nca_ctx.tool_ctx->action |= ACTION_LISTFILES;
                break;
            case 42:
                if (strlen(optarg) > 0) {
                    nca_ctx.tool_ctx->settings.has_expected_content_type = 1;

                    if (strcasecmp(optarg, "program") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTYPE_PROGRAM;
                    } else if (strcasecmp(optarg, "meta") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTYPE_META;
                    } else if (strcasecmp(optarg, "control") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTYPE_CONTROL;
                    } else if (strcasecmp(optarg, "manual") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTYPE_MANUAL;
                    } else if (strcasecmp(optarg, "data") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTYPE_DATA;
                    } else if (strcasecmp(optarg, "publicdata") == 0) {
                        nca_ctx.tool_ctx->settings.expected_content_type = NCACONTENTTPYE_PUBLICDATA;
                    } else if ('0' <= optarg[0] && optarg[1] <= '9') {
                        nca_ctx.tool_ctx->settings.expected_content_type = (optarg[0] - '0');
                    } else {
                        /* Failure to parse expected content type. */
                        addWarning(nca_ctx.tool_ctx, fmt::format("Unknown expected content type ({}).", optarg).data());
                        nca_ctx.tool_ctx->settings.has_expected_content_type = 0;
                    }
                }
                break;
            case 43:
                nca_ctx.tool_ctx->settings.append_section_types = 1;
                break;
            case 44:
                nca_ctx.tool_ctx->settings.suppress_keydata_output = 1;
                break;
            default:
                return stop(nca_ctx.tool_ctx, "Unknown parameter.");
        }
    }

    /* Try to populate default keyfile. */
    FILE *keyfile = NULL;

    if (keypath.valid == VALIDITY_VALID) {
        keyfile = os_fopen(keypath.os_path, OS_MODE_READ);
    }

    FILE *homekeyfile = open_key_file((tool_ctx.action & ACTION_DEV) ? "dev" : "prod");

    if (homekeyfile == NULL) {
        addWarning(nca_ctx.tool_ctx, fmt::format("{}.keys does not exist.", (tool_ctx.action & ACTION_DEV) ? "dev" : "prod").data());
    } else if (keyfile == NULL) {
        keyfile = homekeyfile;
    } else {
        fclose(homekeyfile);
    }

    if (keyfile != NULL) {
        extkeys_initialize_settings(&tool_ctx.settings, keyfile);

        if (tool_ctx.settings.has_sdseed) {
            for (unsigned int key = 0; key < 2; key++) {
                for (unsigned int i = 0; i < 0x20; i++) {
                    tool_ctx.settings.keyset.sd_card_key_sources[key][i] ^= tool_ctx.settings.sdseed[i & 0xF];
                }
            }
        }

        pki_derive_keys(&tool_ctx.settings.keyset);
        fclose(keyfile);
    }

    /* Try to load titlekeys. */
    FILE *titlekeyfile = open_key_file("title");

    if (titlekeyfile != NULL) {
        extkeys_parse_titlekeys(&tool_ctx.settings, titlekeyfile);
    }

    if (optind == argc - 1) {
        /* Copy input filename. */
        strncpy(input_name, argv[optind], sizeof(input_name) - 1);

        addLogLine(nca_ctx.tool_ctx, fmt::format("The source file is {}.", input_name).data());
    } else if (tool_ctx.file_type != FILETYPE_BOOT0 && ((optind < argc) || (argc == 1))) {
        return stop(nca_ctx.tool_ctx, "Provide a source file at the end of your arguments list.");
    }

    cJSON_AddItemToObject(nca_ctx.tool_ctx->output, "source", cJSON_CreateString(input_name));

    /* Special case NAX0. */
    if (tool_ctx.file_type == FILETYPE_NAX0) {
        nax0_ctx_t nax_ctx;
        memset(&nax_ctx, 0, sizeof(nax_ctx));
        filepath_set(&nax_ctx.base_path, input_name);
        nax_ctx.tool_ctx = &tool_ctx;
        nax0_process(&nax_ctx);

        if (nax_ctx.aes_ctx) {
            free_aes_ctx(nax_ctx.aes_ctx);
        }

        if (nax_ctx.num_files) {
            for (unsigned int i = 0; i < nax_ctx.num_files; i++) {
                fclose(nax_ctx.files[i]);
            }
        }

        if (nax_ctx.files) {
            free(nax_ctx.files);
        }

        return finish(nca_ctx.tool_ctx);
    }

    if ((tool_ctx.file = fopen(input_name, "rb")) == NULL && tool_ctx.file_type != FILETYPE_BOOT0) {
        return stop(nca_ctx.tool_ctx, fmt::format("Unable to open the source file {}: {}\n", input_name, strerror(errno)).data());
    }

    switch (tool_ctx.file_type) {
        case FILETYPE_NCA: {
            if (nca_ctx.tool_ctx->base_nca_ctx != NULL) {
                memcpy(&base_ctx.settings.keyset, &tool_ctx.settings.keyset, sizeof(nca_keyset_t));
                base_ctx.settings.known_titlekeys = tool_ctx.settings.known_titlekeys;
                nca_ctx.tool_ctx->base_nca_ctx->tool_ctx = &base_ctx;

                try {
                    nca_process(nca_ctx.tool_ctx->base_nca_ctx, Env);
                } catch (const Napi::TypeError& e) {
                    Napi::TypeError::New(Env, e.what()).ThrowAsJavaScriptException();

                    // Do not continue processing this file.
                    break;
                } catch (const Napi::Error& e) {
                    return stop(nca_ctx.tool_ctx, const_cast<char *>(e.what()));
                }

                int found_romfs = 0;

                for (unsigned int i = 0; i < 4; i++) {
                    if (nca_ctx.tool_ctx->base_nca_ctx->section_contexts[i].is_present && nca_ctx.tool_ctx->base_nca_ctx->section_contexts[i].type == ROMFS) {
                        found_romfs = 1;
                        break;
                    }
                }

                if (found_romfs == 0) {
                    return stop(nca_ctx.tool_ctx, "Unable to locate RomFS in base NCA!");
                }
            }

            nca_ctx.file = tool_ctx.file;

            try {
                nca_process(&nca_ctx, Env);
            } catch (const Napi::TypeError& e) {
                Napi::TypeError::New(Env, e.what()).ThrowAsJavaScriptException();

                // Do not continue processing this file.
                break;
            } catch (const Napi::Error& e) {
                return stop(nca_ctx.tool_ctx, const_cast<char *>(e.what()));
            }

            nca_free_section_contexts(&nca_ctx);

            if (nca_ctx.tool_ctx->base_file_type == BASEFILE_FAKE) {
                nca_ctx.tool_ctx->base_file = NULL;
            }

            if (nca_ctx.tool_ctx->base_file != NULL) {
                fclose(nca_ctx.tool_ctx->base_file);
                if (nca_ctx.tool_ctx->base_file_type == BASEFILE_NCA) {
                    nca_free_section_contexts(nca_ctx.tool_ctx->base_nca_ctx);
                    free(nca_ctx.tool_ctx->base_nca_ctx);
                }
            }
            break;
        }
        case FILETYPE_PFS0: {
            pfs0_ctx_t pfs0_ctx;
            memset(&pfs0_ctx, 0, sizeof(pfs0_ctx));
            pfs0_ctx.file = tool_ctx.file;
            pfs0_ctx.tool_ctx = &tool_ctx;

            try {
                pfs0_process(&pfs0_ctx, Env);
            } catch (const Napi::TypeError& e) {
                Napi::TypeError::New(Env, e.what()).ThrowAsJavaScriptException();

                // Do not continue processing this file.
                break;
            } catch (const Napi::Error& e) {
                return stop(nca_ctx.tool_ctx, const_cast<char *>(e.what()));
            }

            if (pfs0_ctx.header) {
                free(pfs0_ctx.header);
            }

            if (pfs0_ctx.npdm) {
                free(pfs0_ctx.npdm);
            }
            break;
        }
        case FILETYPE_ROMFS: {
            romfs_ctx_t romfs_ctx;
            memset(&romfs_ctx, 0, sizeof(romfs_ctx));
            romfs_ctx.file = tool_ctx.file;
            romfs_ctx.tool_ctx = &tool_ctx;
            romfs_process(&romfs_ctx);

            if (romfs_ctx.files) {
                free(romfs_ctx.files);
            }

            if (romfs_ctx.directories) {
                free(romfs_ctx.directories);
            }
            break;
        }
        case FILETYPE_NCA0_ROMFS: {
            nca0_romfs_ctx_t romfs_ctx;
            memset(&romfs_ctx, 0, sizeof(romfs_ctx));
            romfs_ctx.file = tool_ctx.file;
            romfs_ctx.tool_ctx = &tool_ctx;
            nca0_romfs_process(&romfs_ctx);

            if (romfs_ctx.files) {
                free(romfs_ctx.files);
            }

            if (romfs_ctx.directories) {
                free(romfs_ctx.directories);
            }
            break;
        }
        case FILETYPE_NPDM: {
            npdm_t raw_hdr;
            memset(&raw_hdr, 0, sizeof(raw_hdr));

            if (fread(&raw_hdr, 1, sizeof(raw_hdr), tool_ctx.file) != sizeof(raw_hdr)) {
                return stop(nca_ctx.tool_ctx, "Failed to read NPDM header!");
            }

            if (raw_hdr.magic != MAGIC_META) {
                return stop(nca_ctx.tool_ctx, "NPDM seems corrupt!");
            }

            uint64_t npdm_size = raw_hdr.aci0_size + raw_hdr.aci0_offset;

            if (raw_hdr.acid_offset + raw_hdr.acid_size > npdm_size) {
                npdm_size = raw_hdr.acid_offset + raw_hdr.acid_size;
            }

            fseeko64(tool_ctx.file, 0, SEEK_SET);
            npdm_t *npdm = (npdm_t*) malloc(npdm_size);

            if (npdm == NULL) {
                return stop(nca_ctx.tool_ctx, "Failed to allocate NPDM!");
            }

            if (fread(npdm, 1, npdm_size, tool_ctx.file) != npdm_size) {
                return stop(nca_ctx.tool_ctx, "Failed to read NPDM!");
            }

            npdm_process(npdm, &tool_ctx);
            break;
        }
        case FILETYPE_HFS0: {
            hfs0_ctx_t hfs0_ctx;
            memset(&hfs0_ctx, 0, sizeof(hfs0_ctx));
            hfs0_ctx.file = tool_ctx.file;
            hfs0_ctx.tool_ctx = &tool_ctx;
            hfs0_process(&hfs0_ctx);

            if (hfs0_ctx.header) {
                free(hfs0_ctx.header);
            }
            break;
        }
        case FILETYPE_PACKAGE1: {
            pk11_ctx_t pk11_ctx;
            memset(&pk11_ctx, 0, sizeof(pk11_ctx));
            pk11_ctx.file = tool_ctx.file;
            pk11_ctx.tool_ctx = &tool_ctx;
            pk11_process(&pk11_ctx);

            if (pk11_ctx.pk11) {
                free(pk11_ctx.pk11);
            }
            break;
        }
        case FILETYPE_PACKAGE2: {
            pk21_ctx_t pk21_ctx;
            memset(&pk21_ctx, 0, sizeof(pk21_ctx));
            pk21_ctx.file = tool_ctx.file;
            pk21_ctx.tool_ctx = &tool_ctx;
            pk21_process(&pk21_ctx);

            if (pk21_ctx.sections) {
                free(pk21_ctx.sections);
            }
            break;
        }
        case FILETYPE_INI1: {
            ini1_ctx_t ini1_ctx;
            memset(&ini1_ctx, 0, sizeof(ini1_ctx));
            ini1_ctx.file = tool_ctx.file;
            ini1_ctx.tool_ctx = &tool_ctx;
            ini1_process(&ini1_ctx);

            if (ini1_ctx.header) {
                free(ini1_ctx.header);
            }
            break;
        }
        case FILETYPE_KIP1: {
            kip1_ctx_t kip1_ctx;
            memset(&kip1_ctx, 0, sizeof(kip1_ctx));
            kip1_ctx.file = tool_ctx.file;
            kip1_ctx.tool_ctx = &tool_ctx;
            kip1_process(&kip1_ctx);

            if (kip1_ctx.header) {
                free(kip1_ctx.header);
            }
            break;
        }
        case FILETYPE_NSO0: {
            nso0_ctx_t nso0_ctx;
            memset(&nso0_ctx, 0, sizeof(nso0_ctx));
            nso0_ctx.file = tool_ctx.file;
            nso0_ctx.tool_ctx = &tool_ctx;
            nso0_process(&nso0_ctx);

            if (nso0_ctx.header) {
                free(nso0_ctx.header);
            }

            if (nso0_ctx.uncompressed_header) {
                free(nso0_ctx.uncompressed_header);
            }
            break;
        }
        case FILETYPE_XCI: {
            xci_ctx_t xci_ctx;
            memset(&xci_ctx, 0, sizeof(xci_ctx));
            xci_ctx.file = tool_ctx.file;
            xci_ctx.tool_ctx = &tool_ctx;

            try {
                xci_process(&xci_ctx, Env);
            } catch (const Napi::TypeError& e) {
                Napi::TypeError::New(Env, e.what()).ThrowAsJavaScriptException();

                // Do not continue processing this file.
                break;
            } catch (const Napi::Error& e) {
                return stop(nca_ctx.tool_ctx, const_cast<char *>(e.what()));
            }

            break;
        }
        case FILETYPE_BOOT0: {
            nca_keyset_t new_keyset;
            memcpy(&new_keyset, &tool_ctx.settings.keyset, sizeof(new_keyset));

            for (unsigned int i = 0; i < 0x10; i++) {
                if (tool_ctx.settings.keygen_sbk[i] != 0) {
                    memcpy(new_keyset.secure_boot_key, tool_ctx.settings.keygen_sbk, 0x10);
                }
            }

            for (unsigned int i = 0; i < 0x10; i++) {
                if (tool_ctx.settings.keygen_tsec[i] != 0) {
                    memcpy(new_keyset.tsec_key, tool_ctx.settings.keygen_tsec, 0x10);
                }
            }

            for (unsigned int i = 0; tool_ctx.file != NULL && i < 0x20; i++) {
                fseek(tool_ctx.file, 0x180000 + 0x200 * i, SEEK_SET);

                if (fread(&new_keyset.encrypted_keyblobs[i], sizeof(new_keyset.encrypted_keyblobs[i]), 1, tool_ctx.file) != 1) {
                    // Previous format substitution was %02x.
                    return stop(nca_ctx.tool_ctx, fmt::format("Failed to read encrypted_keyblob_{} from boot0!", i).data());
                }
            }

            pki_derive_keys(&new_keyset);
            // printf("All derivable keys (using loaded sources):\n\n");
            pki_print_keys(&new_keyset);
            break;
        }
        case FILETYPE_SAVE: {
            save_ctx_t save_ctx;
            memset(&save_ctx, 0, sizeof(save_ctx));
            save_ctx.file = tool_ctx.file;
            save_ctx.tool_ctx = &tool_ctx;
            save_process(&save_ctx);
            save_free_contexts(&save_ctx);
            break;
        }
        default: {
            return stop(nca_ctx.tool_ctx, "Unknown file type.");
        }
    }

    if (tool_ctx.settings.known_titlekeys.titlekeys != NULL) {
        free(tool_ctx.settings.known_titlekeys.titlekeys);
    }

    if (tool_ctx.file != NULL) {
        fclose(tool_ctx.file);
    }

    char *output = finish(nca_ctx.tool_ctx);

    return output;
}
