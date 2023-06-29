#include <string.h>
#include <fmt/core.h>
#include "pfs0.h"
#include "output.h"

void pfs0_process(pfs0_ctx_t *ctx, Napi::Env Env) {
    /* Read *just* safe amount. */
    pfs0_header_t raw_header;
    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(&raw_header, 1, sizeof(raw_header), ctx->file) != sizeof(raw_header)) {
        throw Napi::Error::New(Env, "Failed to read PFS0 header!");
    }

    if (raw_header.magic != MAGIC_PFS0) {
        throw Napi::Error::New(Env, "PFS0 is corrupt!");
    }

    uint64_t header_size = pfs0_get_header_size(&raw_header);
    ctx->header = static_cast<pfs0_header_t *>(malloc(header_size));
    if (ctx->header == NULL) {
        throw Napi::Error::New(Env, "Failed to allocate PFS0 header!");
    }

    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(ctx->header, 1, header_size, ctx->file) != header_size) {
        throw Napi::Error::New(Env, "Failed to read PFS0 header!");
    }

    /* Weak file validation. */
    uint64_t max_size = 0x1ULL;
    max_size <<= 48; /* Switch file sizes are capped at 48 bits. */
    uint64_t cur_ofs = 0;

    for (unsigned int i = 0; i < ctx->header->num_files; i++) {
        pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->header, i);
        cur_ofs += cur_file->size;
    }

    for (unsigned int i = 0; i < ctx->header->num_files; i++) {
        pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->header, i);

        if (strcmp(pfs0_get_file_name(ctx->header, i), "main.npdm") == 0) {
            /* We might have found the exefs. */

            ctx->npdm = static_cast<npdm_t *>(malloc(cur_file->size));

            if (ctx->npdm == NULL) {
                throw Napi::Error::New(Env, "Failed to allocate NPDM!");
            }

            fseeko64(ctx->file, pfs0_get_header_size(ctx->header) + cur_file->offset, SEEK_SET);

            if (fread(ctx->npdm, 1, cur_file->size, ctx->file) != cur_file->size) {
                throw Napi::Error::New(Env, "Failed to read NPDM!");
            }

            if (ctx->npdm->magic == MAGIC_META) {
                ctx->is_exefs = 1;
            }
        }
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        pfs0_print(ctx, Env);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        pfs0_save(ctx, Env);
    }
}

static void pfs0_save_file(pfs0_ctx_t *ctx, uint32_t i, filepath_t *dirpath, Napi::Env Env) {
    if (i >= ctx->header->num_files) {
        throw Napi::Error::New(Env, fmt::format("Could not save file {}!", i));
    }

    pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->header, i);

    if (strlen(pfs0_get_file_name(ctx->header, i)) >= MAX_PATH - strlen(dirpath->char_path) - 1) {
        throw Napi::Error::New(Env, fmt::format("Filename too long in PFS0!", i));
    }

    filepath_t filepath;
    filepath_copy(&filepath, dirpath);
    filepath_append(&filepath, "%s", pfs0_get_file_name(ctx->header, i));

    cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Saving {}.", filepath.char_path).data()));

    uint64_t ofs = pfs0_get_header_size(ctx->header) + cur_file->offset;
    save_file_section(ctx->file, ofs, cur_file->size, &filepath);
}

// Extract files to a directory.
void pfs0_save(pfs0_ctx_t *ctx, Napi::Env Env) {
    filepath_t *dirpath = NULL;

    if (ctx->is_exefs && ctx->tool_ctx->settings.exefs_dir_path.enabled) {
        dirpath = &ctx->tool_ctx->settings.exefs_dir_path.path;
    }

    if ((dirpath == NULL || dirpath->valid != VALIDITY_VALID) && (ctx->tool_ctx->file_type == FILETYPE_PFS0 && ctx->tool_ctx->settings.out_dir_path.enabled)) {
        dirpath = &ctx->tool_ctx->settings.out_dir_path.path;
    }

    if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
        dirpath = &ctx->tool_ctx->settings.pfs0_dir_path;
    }

    if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
        os_makedir(dirpath->os_path);

        cJSON *extracted_files = cJSON_CreateArray();
        cJSON_AddItemToObject(ctx->tool_ctx->output, "extracted", extracted_files);

        for (uint32_t i = 0; i < ctx->header->num_files; i++) {
            bool single = ctx->tool_ctx->settings.single_file.enabled;
            bool found = strcmp(ctx->tool_ctx->settings.single_file.path.char_path, pfs0_get_file_name(ctx->header, i)) == 0;

            if (!single || (single && found)) {
                pfs0_save_file(ctx, i, dirpath, Env);

                cJSON *file_details = cJSON_CreateObject();
                cJSON_AddItemToObject(file_details, "name", cJSON_CreateString(pfs0_get_file_name(ctx->header, i)));
                cJSON_AddItemToObject(file_details, "path", cJSON_CreateString(dirpath->char_path));
                cJSON_AddItemToArray(extracted_files, file_details);
            }
        }
    }
}

void pfs0_print(pfs0_ctx_t *ctx, Napi::Env Env) {
    char *file_system = ctx->is_exefs ? "ExeFS" : "PFS0";
    char *magic = return_magic(ctx->header->magic);

    // Create an object to hold the archive details.
    cJSON *archive_details = cJSON_CreateObject();
    cJSON_AddItemToObject(ctx->tool_ctx->output, "archive", archive_details);

    // Add the filesystem to the archive details.
    cJSON_AddItemToObject(archive_details, "fileSystem", cJSON_CreateString(file_system));
    cJSON_AddItemToObject(archive_details, "magic", cJSON_CreateString(magic));

    free(magic);

    if (ctx->is_exefs) {
        cJSON_AddItemToObject(archive_details, "titleId", cJSON_CreateNumber(npdm_get_aci0(ctx->npdm)->title_id));
    }

    cJSON *files = cJSON_CreateArray();
    cJSON_AddItemToObject(archive_details, "files", files);

    // Arbitrary file number limit.
    if (ctx->header->num_files > 0 && ctx->header->num_files < 20) {
        for (unsigned int i = 0; i < ctx->header->num_files; i++) {
            pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->header, i);

            char *name = pfs0_get_file_name(ctx->header, i);

            cJSON *details = cJSON_CreateObject();
            cJSON_AddItemToArray(files, details);

            cJSON_AddItemToObject(details, "name", cJSON_CreateString(name));
            cJSON_AddItemToObject(details, "path", cJSON_CreateString(fmt::format("pfs0:/{}", name).data()));
            cJSON_AddItemToObject(details, "offsetStart", cJSON_CreateNumber(cur_file->offset));
            cJSON_AddItemToObject(details, "offsetEnd", cJSON_CreateNumber(cur_file->offset + cur_file->size));
        }
    }

    if (ctx->is_exefs) {
        npdm_process(ctx->npdm, ctx->tool_ctx);
    }
}
