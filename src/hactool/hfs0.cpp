#include <string.h>
#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include "hfs0.h"

void hfs0_process(hfs0_ctx_t *ctx, Napi::Env Env) {
    cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString("Processing HFS0 file."));

    /* Read *just* safe amount. */
    hfs0_header_t raw_header;
    fseeko64(ctx->file, ctx->offset, SEEK_SET);

    if (fread(&raw_header, 1, sizeof(raw_header), ctx->file) != sizeof(raw_header)) {
        throw Napi::TypeError::New(Env, "Failed to read HFS0 header.");
    }

    if (raw_header.magic != MAGIC_HFS0) {
        memdump(stdout, "Sanity: ", &raw_header, sizeof(raw_header));

        throw Napi::TypeError::New(Env, "HFS0 is corrupt.");
    }

    uint64_t header_size = hfs0_get_header_size(&raw_header);
    ctx->header = static_cast<hfs0_header_t *>(malloc(header_size));

    if (ctx->header == NULL) {
        throw Napi::TypeError::New(Env, "Failed to allocate HFS0 header.");
    }

    fseeko64(ctx->file, ctx->offset, SEEK_SET);

    if (fread(ctx->header, 1, header_size, ctx->file) != header_size) {
        throw Napi::TypeError::New(Env, "Failed to read HFS0 header.");
    }

    /* Weak file validation. */
    uint64_t max_size = 0x1ULL;
    max_size <<= 48; /* Switch file sizes are capped at 48 bits. */
    uint64_t cur_ofs = 0;

    for (unsigned int i = 0; i < ctx->header->num_files; i++) {
        hfs0_file_entry_t *cur_file = hfs0_get_file_entry(ctx->header, i);

        if (cur_file->offset < cur_ofs) {
            throw Napi::TypeError::New(Env, "HFS0 is corrupt.");
        }

        cur_ofs += cur_file->size;
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        hfs0_print(ctx, Env);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        hfs0_save(ctx, Env);
    }
}

void hfs0_save_file(hfs0_ctx_t *ctx, uint32_t i, filepath_t *dirpath, Napi::Env Env, cJSON *file_details) {
    if (i >= ctx->header->num_files) {
        throw Napi::Error::New(Env, fmt::format("Could not save file", i).data());
    }

    hfs0_file_entry_t *cur_file = hfs0_get_file_entry(ctx->header, i);

    if (strlen(hfs0_get_file_name(ctx->header, i)) >= MAX_PATH - strlen(dirpath->char_path) - 1) {
        throw Napi::Error::New(Env, "Filename too long in HFS0.");
    }

    filepath_t filepath;
    filepath_copy(&filepath, dirpath);
    filepath_append(&filepath, "%s", hfs0_get_file_name(ctx->header, i));

    cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Saving {} to {}", hfs0_get_file_name(ctx->header, i), filepath.char_path).data()));

    cJSON_AddItemToObject(file_details, "name", cJSON_CreateString(hfs0_get_file_name(ctx->header, i)));
    cJSON_AddItemToObject(file_details, "path", cJSON_CreateString(filepath.char_path));

    uint64_t ofs = hfs0_get_header_size(ctx->header) + cur_file->offset;
    save_file_section(ctx->file, ctx->offset + ofs, cur_file->size, &filepath);
}

void hfs0_save(hfs0_ctx_t *ctx, Napi::Env Env) {
    cJSON *extracted_files = cJSON_CreateArray();
    cJSON_AddItemToObject(ctx->tool_ctx->output, "extracted", extracted_files);

    /* Extract to directory. */
    filepath_t *dirpath = NULL;

    if (ctx->tool_ctx->file_type == FILETYPE_HFS0 && ctx->tool_ctx->settings.out_dir_path.enabled) {
        dirpath = &ctx->tool_ctx->settings.out_dir_path.path;
    }

    if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
        dirpath = &ctx->tool_ctx->settings.hfs0_dir_path;
    }

    if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
        os_makedir(dirpath->os_path);

        for (uint32_t i = 0; i < ctx->header->num_files; i++) {
            cJSON *file_details = cJSON_CreateObject();
            cJSON_AddItemToArray(extracted_files, file_details);

            hfs0_save_file(ctx, i, dirpath, Env, file_details);
        }
    }
}

void hfs0_print(hfs0_ctx_t *ctx, Napi::Env Env) {
    // Create an object to hold the archive details.
    cJSON *archive_details = cJSON_CreateObject();
    cJSON_AddItemToObject(ctx->tool_ctx->output, "archive", archive_details);

    printf("\nHFS0:\n");
    print_magic("Magic:                              ", ctx->header->magic);
    printf("Number of files:                    %" PRId32 "\n", ctx->header->num_files);

    if (ctx->header->num_files > 0) {
        printf("Files:");

        for (unsigned int i = 0; i < ctx->header->num_files; i++) {
            hfs0_file_entry_t *cur_file = hfs0_get_file_entry(ctx->header, i);

            if (ctx->tool_ctx->action & ACTION_VERIFY) {
                validity_t hash_validity = check_memory_hash_table_with_suffix(ctx->file, cur_file->hash, ctx->offset + hfs0_get_header_size(ctx->header) + cur_file->offset, cur_file->hashed_size, cur_file->hashed_size, ctx->hash_suffix, 0);
                printf("%s%s:/%-48s %012" PRIx64 "-%012" PRIx64 " (%s)\n", i == 0 ? "                              " : "                                    ", ctx->name == NULL ? "hfs0" : ctx->name, hfs0_get_file_name(ctx->header, i), cur_file->offset, cur_file->offset + cur_file->size, GET_VALIDITY_STR(hash_validity));
            } else {
                printf("%s%s:/%-48s %012" PRIx64 "-%012" PRIx64 "\n", i == 0 ? "                              " : "                                    ", ctx->name == NULL ? "hfs0" : ctx->name, hfs0_get_file_name(ctx->header, i), cur_file->offset, cur_file->offset + cur_file->size);
            }
        }
    }
}
