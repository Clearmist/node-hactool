#include <stdlib.h>
#include <string.h>
#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include <cJSON.h>
#include "nca.h"
#include "aes.h"
#include "pki.h"
#include "sha.h"
#include "rsa.h"
#include "utils.h"
#include "extkeys.h"
#include "filepath.h"

/* Initialize the context. */
void nca_init(nca_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* Updates the CTR for an offset. */
static void nca_update_ctr(unsigned char *ctr, uint64_t ofs) {
    ofs >>= 4;
    for (unsigned int j = 0; j < 0x8; j++) {
        ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
        ofs >>= 8;
    }
}

/* Updates the CTR for a bktr offset. */
static void nca_update_bktr_ctr(unsigned char *ctr, uint32_t ctr_val, uint64_t ofs) {
    ofs >>= 4;
    for (unsigned int j = 0; j < 0x8; j++) {
        ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
        ofs >>= 8;
    }
    for (unsigned int j = 0; j < 4; j++) {
        ctr[0x8-j-1] = (unsigned char)(ctr_val & 0xFF);
        ctr_val >>= 8;
    }
}

/* Seek to an offset within a section. */
void nca_section_fseek(nca_section_ctx_t *ctx, uint64_t offset) {
    if (ctx->is_decrypted) {
        fseeko64(ctx->file, (ctx->offset + offset), SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset);
    } else if (ctx->crypt_type == CRYPT_XTS) {
        fseeko64(ctx->file, (ctx->offset + offset) & ~ctx->sector_mask, SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset) & ~ctx->sector_mask;
        ctx->sector_num = offset / ctx->sector_size;
        ctx->sector_ofs = offset & ctx->sector_mask;
    } else if (ctx->crypt_type == CRYPT_NCA0) {
        fseeko64(ctx->file, (ctx->offset + offset) & ~ctx->sector_mask, SEEK_SET);
        ctx->cur_seek = ((ctx->offset + offset - 0x400ULL) & ~ctx->sector_mask) + 0x400ULL;
        ctx->sector_num = (ctx->offset + offset - 0x400ULL) / ctx->sector_size;
        ctx->sector_ofs = (ctx->offset + offset - 0x400ULL) & ctx->sector_mask;
    } else if (ctx->type == BKTR && ctx->bktr_ctx.subsection_block != NULL) {
        /* No better way to do this than to make all BKTR seeking virtual. */
        ctx->bktr_ctx.virtual_seek = offset;
        if (ctx->tool_ctx->base_file == NULL && ctx->physical_reads == 0) { /* Without base romfs, reads will be physical. */
            ctx->bktr_ctx.bktr_seek = offset;
        } else { /* Let's do the complicated thing. */
            bktr_relocation_entry_t *reloc = bktr_get_relocation(ctx->bktr_ctx.relocation_block, offset);
            uint64_t section_ofs = offset - reloc->virt_offset + reloc->phys_offset;
            if (reloc->is_patch) {
                /* Seeked within the patch romfs. */
                ctx->bktr_ctx.bktr_seek = section_ofs;
            } else {
                /* Seeked within the base romfs. */
                ctx->bktr_ctx.base_seek = section_ofs;
            }
        }
    } else if (ctx->crypt_type != CRYPT_NONE) { /* CTR, and BKTR until subsections are read. */
        fseeko64(ctx->file, (ctx->offset + offset) & ~0xF, SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset) & ~0xF;
        nca_update_ctr(ctx->ctr, ctx->offset + offset);
        ctx->sector_ofs = offset & 0xF;
    }
}

static size_t nca_bktr_section_physical_fread(nca_section_ctx_t *ctx, void *buffer, size_t count, Napi::Env Env) {
    size_t read = 0; /* XXX */
    size_t size = 1;
    char block_buf[0x10];

    if (ctx->is_decrypted) {
        fseeko64(ctx->file, (ctx->offset + ctx->bktr_ctx.bktr_seek), SEEK_SET);
        read = fread(buffer, size, count, ctx->file);
        nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + read);
        return read;
    }

    bktr_subsection_entry_t *subsec = bktr_get_subsection(ctx->bktr_ctx.subsection_block, ctx->bktr_ctx.bktr_seek);
    nca_update_bktr_ctr(ctx->ctr, subsec->ctr_val, ctx->bktr_ctx.bktr_seek + ctx->offset);
    fseeko64(ctx->file, (ctx->offset + ctx->bktr_ctx.bktr_seek) & ~0xF, SEEK_SET);
    uint32_t block_ofs;
    bktr_subsection_entry_t *next_subsec = subsec + 1;
    if (ctx->bktr_ctx.bktr_seek + count <= next_subsec->offset) {
        /* Easy path, reading *only* within the subsection. */
        if ((block_ofs = ctx->bktr_ctx.bktr_seek & 0xF) != 0) {
            if ((read = fread(block_buf, 1, 0x10, ctx->file)) != 0x10) {
                return 0;
            }
            aes_setiv(ctx->aes, ctx->ctr, 0x10);
            aes_decrypt(ctx->aes, block_buf, block_buf, 0x10);
            if (count + block_ofs < 0x10) {
                memcpy(buffer, block_buf + ctx->sector_ofs, count);
                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);
                return count;
            }
            memcpy(buffer, block_buf + block_ofs, 0x10 - block_ofs);
            uint32_t read_in_block = 0x10 - block_ofs;
            nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek - block_ofs + 0x10);
            return read_in_block + nca_section_fread(ctx, (char *)buffer + read_in_block, count - read_in_block, Env);
        }
        if ((read = fread(buffer, 1, count, ctx->file)) != count) {
                return 0;
        }
        aes_setiv(ctx->aes, ctx->ctr, 16);
        aes_decrypt(ctx->aes, buffer, buffer, count);
        nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);
    } else {
        /* Sad path. */
        uint64_t within_subsection = next_subsec->offset - ctx->bktr_ctx.bktr_seek;
        if ((read = nca_section_fread(ctx, buffer, within_subsection, Env)) != within_subsection) {
            return 0;
        }
        read += nca_section_fread(ctx, (char *)buffer + within_subsection, count - within_subsection, Env);
        if (read != count) {
            return 0;
        }
    }

    return read;
}

size_t nca_section_fread(nca_section_ctx_t *ctx, void *buffer, size_t count, Napi::Env Env) {
    size_t read = 0; /* XXX */
    size_t size = 1;
    char block_buf[0x10];

    if (ctx->is_decrypted && ctx->type != BKTR) {
        read = fread(buffer, size, count, ctx->file);
        return read;
    }

    if (ctx->crypt_type == CRYPT_XTS || ctx->crypt_type == CRYPT_NCA0) { /* AES-XTS requires special handling... */
        unsigned char *sector_buf = static_cast<unsigned char *>(malloc(ctx->sector_size));
        if ((read = fread(sector_buf, size, ctx->sector_size, ctx->file)) != ctx->sector_size) {
            free(sector_buf);
            return 0;
        }
        aes_xts_decrypt(ctx->aes, sector_buf, sector_buf, ctx->sector_size, ctx->sector_num, ctx->sector_size);
        if (count > ctx->sector_size - ctx->sector_ofs) { /* We're leaving the sector... */
            memcpy(buffer, sector_buf + ctx->sector_ofs, ctx->sector_size - ctx->sector_ofs);
            size_t remaining = count - (ctx->sector_size - ctx->sector_ofs);
            size_t ofs = (ctx->sector_size - ctx->sector_ofs);
            ctx->sector_num++;
            ctx->sector_ofs = 0;
            if (remaining & ~ctx->sector_mask) { /* Read intermediate sectors. */
                uint64_t addl;
                if ((addl = fread((char *)buffer + ofs, size, (remaining & ~ctx->sector_mask), ctx->file)) != (remaining & ~ctx->sector_mask)) {
                    free(sector_buf);
                    return ofs;
                }

                aes_xts_decrypt(ctx->aes, (char *)buffer + ofs, (char *)buffer + ofs, remaining & ~ctx->sector_mask, ctx->sector_num, ctx->sector_size);
                ctx->sector_num += remaining / ctx->sector_size;
                ofs += remaining & ~ctx->sector_mask;
                remaining &= ctx->sector_mask;
                read += addl;
            }
            if (remaining) { /* Read last sector. */
                if ((read = fread(sector_buf, size, ctx->sector_size, ctx->file)) != ctx->sector_size) {
                    free(sector_buf);
                    return ofs;
                }
                aes_xts_decrypt(ctx->aes, sector_buf, sector_buf, ctx->sector_size, ctx->sector_num, ctx->sector_size);
                memcpy((char *)buffer + ofs, sector_buf, remaining);
                ctx->sector_ofs = remaining;
                read = count;
            }
        } else {
            memcpy(buffer, sector_buf + ctx->sector_ofs, count);
            ctx->sector_num += (ctx->sector_ofs + count) / ctx->sector_size;
            ctx->sector_ofs += count;
            ctx->sector_ofs &= ctx->sector_mask;
            read = count;
        }
        free(sector_buf);
    } else {
        /* Perform decryption, if necessary. */
        /* AES-CTR. */
        if (ctx->crypt_type == CRYPT_CTR || (ctx->crypt_type == CRYPT_BKTR && ctx->bktr_ctx.subsection_block == NULL))
        {
            if (ctx->sector_ofs) {
                if ((read = fread(block_buf, 1, 0x10, ctx->file)) != 0x10) {
                    return 0;
                }
                aes_setiv(ctx->aes, ctx->ctr, 0x10);
                aes_decrypt(ctx->aes, block_buf, block_buf, 0x10);
                if (count + ctx->sector_ofs < 0x10) {
                    memcpy(buffer, block_buf + ctx->sector_ofs, count);
                    ctx->sector_ofs += count;
                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + ctx->sector_ofs);
                    return count;
                }
                memcpy(buffer, block_buf + ctx->sector_ofs, 0x10 - ctx->sector_ofs);
                uint32_t read_in_block = 0x10 - ctx->sector_ofs;
                nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + 0x10);
                return read_in_block + nca_section_fread(ctx, (char *)buffer + read_in_block, count - read_in_block, Env);
            }
            if ((read = fread(buffer, 1, count, ctx->file)) != count) {
                return 0;
            }
            aes_setiv(ctx->aes, ctx->ctr, 16);
            aes_decrypt(ctx->aes, buffer, buffer, count);
            nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + count);
        } else if (ctx->crypt_type == CRYPT_BKTR) { /* Spooky BKTR AES-CTR. */
            /* Are we doing virtual reads, or physical reads? */
            if (ctx->tool_ctx->base_file != NULL && ctx->physical_reads == 0) {
                bktr_relocation_entry_t *reloc = bktr_get_relocation(ctx->bktr_ctx.relocation_block, ctx->bktr_ctx.virtual_seek);
                bktr_relocation_entry_t *next_reloc = reloc + 1;
                uint64_t virt_seek = ctx->bktr_ctx.virtual_seek;
                if (ctx->bktr_ctx.virtual_seek + count <= next_reloc->virt_offset) {
                    /* Easy path: We're reading *only* within the current relocation. */
                    if (reloc->is_patch) {
                        read = nca_bktr_section_physical_fread(ctx, buffer, count, Env);
                    } else {
                        /* Nice and easy read from the base rom. */
                        if (ctx->tool_ctx->base_file_type == BASEFILE_ROMFS) {
                            fseeko64(ctx->tool_ctx->base_file, ctx->bktr_ctx.base_seek, SEEK_SET);
                            if ((read = fread(buffer, 1, count, ctx->tool_ctx->base_file)) != count) {
                                return 0;
                            }
                        } else if (ctx->tool_ctx->base_file_type == BASEFILE_NCA) {
                            nca_ctx_t *base_ctx = ctx->tool_ctx->base_nca_ctx;
                            unsigned int romfs_section_num;
                            for (romfs_section_num = 0; romfs_section_num < 4; romfs_section_num++) {
                                if (base_ctx->section_contexts[romfs_section_num].type == ROMFS) {
                                    break;
                                }
                            }
                            nca_section_fseek(&base_ctx->section_contexts[romfs_section_num], ctx->bktr_ctx.base_seek);
                            if ((read = nca_section_fread(&base_ctx->section_contexts[romfs_section_num], buffer, count, Env)) != count) {
                                throw Napi::Error::New(Env, "Failed to read from Base NCA RomFS.");
                            }
                        } else if (ctx->tool_ctx->base_file_type == BASEFILE_FAKE) {
                            /* Fake reads. */
                            memset(buffer, 0xCC, count);
                            read = count;
                        } else {
                            throw Napi::Error::New(Env, "Unknown base file type.");
                        }
                    }
                } else {
                    uint64_t within_relocation = next_reloc->virt_offset - ctx->bktr_ctx.virtual_seek;
                    if ((read = nca_section_fread(ctx, buffer, within_relocation, Env)) != within_relocation) {
                        return 0;
                    }
                    nca_section_fseek(ctx, virt_seek + within_relocation);
                    read += nca_section_fread(ctx, (char *)buffer + within_relocation, count - within_relocation, Env);
                    if (read != count) {
                        return 0;
                    }
                }
                nca_section_fseek(ctx, virt_seek + count);
            } else {
                read = nca_bktr_section_physical_fread(ctx, buffer, count, Env);
            }
        }
    }
    return read;
}

void nca_free_section_contexts(nca_ctx_t *ctx) {
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            if (ctx->section_contexts[i].aes) {
                free_aes_ctx(ctx->section_contexts[i].aes);
            }
            if (ctx->section_contexts[i].type == PFS0 && ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                free(ctx->section_contexts[i].pfs0_ctx.npdm);
            } else if (ctx->section_contexts[i].type == ROMFS) {
                if (ctx->section_contexts[i].romfs_ctx.directories) {
                    free(ctx->section_contexts[i].romfs_ctx.directories);
                }
                if (ctx->section_contexts[i].romfs_ctx.files) {
                    free(ctx->section_contexts[i].romfs_ctx.files);
                }
            }  else if (ctx->section_contexts[i].type == NCA0_ROMFS) {
                if (ctx->section_contexts[i].nca0_romfs_ctx.directories) {
                    free(ctx->section_contexts[i].nca0_romfs_ctx.directories);
                }
                if (ctx->section_contexts[i].nca0_romfs_ctx.files) {
                    free(ctx->section_contexts[i].nca0_romfs_ctx.files);
                }
            } else if (ctx->section_contexts[i].type == BKTR) {
                if (ctx->section_contexts[i].bktr_ctx.subsection_block) {
                    free(ctx->section_contexts[i].bktr_ctx.subsection_block);
                }
                if (ctx->section_contexts[i].bktr_ctx.relocation_block) {
                    free(ctx->section_contexts[i].bktr_ctx.relocation_block);
                }
                if (ctx->section_contexts[i].bktr_ctx.directories) {
                    free(ctx->section_contexts[i].bktr_ctx.directories);
                }
                if (ctx->section_contexts[i].bktr_ctx.files) {
                    free(ctx->section_contexts[i].bktr_ctx.files);
                }
            }
        }
    }
}

static const char *nca_get_section_type_name(enum nca_section_type type) {
    switch (type) {
        case PFS0:
            return "pfs0";
        case ROMFS:
        case BKTR:
        case NCA0_ROMFS:
            return "romfs";
        default:
            return "unknown";
    }
}

static void nca_save(nca_ctx_t *ctx, Napi::Env Env) {
    /* Save header. */
    filepath_t *header_path = &ctx->tool_ctx->settings.header_path;

    if (header_path->valid == VALIDITY_VALID) {
        cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Saving Header to {}.", header_path->char_path).data()));

        FILE *f_hdr = os_fopen(header_path->os_path, OS_MODE_WRITE);

        if (f_hdr != NULL) {
            fwrite(&ctx->header, 1, 0xC00, f_hdr);
            fclose(f_hdr);
        } else {
            cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString(fmt::format("Failed to open {}.", header_path->char_path).data()));
        }
    }

    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            nca_save_section(&ctx->section_contexts[i], Env);
        }
    }

    /* Save Decrypted NCA. */
    filepath_t *dec_path = &ctx->tool_ctx->settings.plaintext_path;

    if (dec_path->valid == VALIDITY_VALID) {
        cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Saving decrypted NCA to {}.", dec_path->char_path).data()));

        FILE *f_dec = os_fopen(dec_path->os_path, OS_MODE_WRITE);

        if (f_dec != NULL) {
            if (fwrite(&ctx->header, 1, 0xC00, f_dec) != 0xC00) {
                throw Napi::Error::New(Env, "Failed to write header.");
            }

            unsigned char *buf = static_cast<unsigned char *>(malloc(0x400000));

            if (buf == NULL) {
                throw Napi::Error::New(Env, "Failed to allocate file-save buffer.");
            }

            for (unsigned int i = 0; i < 4; i++) {
                if (ctx->section_contexts[i].is_present) {
                    fseeko64(f_dec, ctx->section_contexts[i].offset, SEEK_SET);
                    ctx->section_contexts[i].physical_reads = 1;

                    uint64_t read_size = 0x400000; /* 4 MB buffer. */
                    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
                    uint64_t ofs = 0;
                    uint64_t end_ofs = ofs + ctx->section_contexts[i].size;
                    nca_section_fseek(&ctx->section_contexts[i], ofs);

                    while (ofs < end_ofs) {
                        if (ofs + read_size >= end_ofs) {
                            read_size = end_ofs - ofs;
                        }

                        if (nca_section_fread(&ctx->section_contexts[i], buf, read_size, Env) != read_size) {
                            throw Napi::Error::New(Env, "Failed to read file.");
                        }

                        if (fwrite(buf, 1, read_size, f_dec) != read_size) {
                            throw Napi::Error::New(Env, "Failed to write file.");
                        }

                        ofs += read_size;
                    }

                    ctx->section_contexts[i].physical_reads = 0;
                }
            }

            fclose(f_dec);

            free(buf);
        } else {
            cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString(fmt::format("Failed to open {}.", dec_path->char_path).data()));
        }
    }
}

void nca_process(nca_ctx_t *ctx, Napi::Env Env) {
    cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString("Processing NCA file."));

    /* First things first, decrypt header. */
    if (!nca_decrypt_header(ctx, Env)) {
        throw Napi::TypeError::New(Env, "Failed to read the NCA header. Are the keys correct?");
    }

    if (ctx->header.fixed_key_generation < sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli) / sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[0])) {
        if (rsa2048_pss_verify(&ctx->header.magic, 0x200, ctx->header.fixed_key_sig, ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[ctx->header.fixed_key_generation])) {
            ctx->fixed_sig_validity = VALIDITY_VALID;
        } else {
            ctx->fixed_sig_validity = VALIDITY_INVALID;
        }
    } else {
        ctx->fixed_sig_validity = VALIDITY_INVALID;
    }

    /* Sort out crypto type. */
    ctx->crypto_type = ctx->header.crypto_type;
    if (ctx->header.crypto_type2 > ctx->header.crypto_type) {
        ctx->crypto_type = ctx->header.crypto_type2;
    }

    if (ctx->crypto_type) {
        ctx->crypto_type--; /* 0, 1 are both master key 0. */
    }

    /* Rights ID. */
    for (unsigned int i = 0; i < 0x10; i++) {
        if (ctx->header.rights_id[i] != 0) {
            ctx->has_rights_id = 1;
            break;
        }
    }

    if (ctx->is_cli_target && ctx->tool_ctx->base_nca_ctx != NULL) {
        uint64_t base_tid = ctx->tool_ctx->base_nca_ctx->header.title_id;
        uint64_t expectation = ctx->header.title_id & 0xFFFFFFFFFFFFF7FFULL;
        if (base_tid != expectation) {
            char * message = fmt::format("Base NCA Title ID doesn't match expectation ({} != {})", base_tid, expectation).data();
            cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString(message));
        }
    }

    /* Enforce content type for extraction if required. */
    if (ctx->tool_ctx->settings.has_expected_content_type) {
        if (ctx->tool_ctx->settings.expected_content_type != ctx->header.content_type) {
            ctx->tool_ctx->action &= ~(ACTION_EXTRACT);
        }
    }

    /* Decrypt key area if required. */
    if (!ctx->has_rights_id) {
        nca_decrypt_key_area(ctx);
    } else {
        /* Decrypt title key. */
        aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.titlekeks[ctx->crypto_type], 16, AES_MODE_ECB);

        if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
            aes_decrypt(aes_ctx, ctx->tool_ctx->settings.dec_cli_titlekey, ctx->tool_ctx->settings.cli_titlekey, 0x10);
        } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
            titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
            aes_decrypt(aes_ctx, entry->dec_titlekey, entry->titlekey, 0x10);
        }

        free_aes_ctx(aes_ctx);
    }

    /* Parse sections. */
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->header.section_entries[i].media_start_offset) { /* Section exists. */
            ctx->section_contexts[i].is_present = 1;
            ctx->section_contexts[i].is_decrypted = ctx->is_decrypted;
            ctx->section_contexts[i].tool_ctx = ctx->tool_ctx;
            ctx->section_contexts[i].file = ctx->file;
            ctx->section_contexts[i].section_num = i;
            ctx->section_contexts[i].offset = media_to_real(ctx->header.section_entries[i].media_start_offset);
            ctx->section_contexts[i].size = media_to_real(ctx->header.section_entries[i].media_end_offset) - ctx->section_contexts[i].offset;
            ctx->section_contexts[i].header = &ctx->header.fs_headers[i];
            ctx->section_contexts[i].crypt_type = static_cast<section_crypt_type_t>(ctx->section_contexts[i].header->crypt_type);

            if (ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA) {
                ctx->section_contexts[i].crypt_type = CRYPT_NCA0;
            }

            if (ctx->section_contexts[i].header->partition_type == PARTITION_PFS0 && ctx->section_contexts[i].header->fs_type == FS_TYPE_PFS0) {
                ctx->section_contexts[i].type = PFS0;
                ctx->section_contexts[i].pfs0_ctx.superblock = &ctx->section_contexts[i].header->pfs0_superblock;
            } else if (ctx->section_contexts[i].header->partition_type == PARTITION_ROMFS && ctx->section_contexts[i].header->fs_type == FS_TYPE_ROMFS) {
                if (ctx->section_contexts[i].crypt_type == CRYPT_BKTR) {
                    ctx->section_contexts[i].type = BKTR;
                    ctx->section_contexts[i].bktr_ctx.superblock = &ctx->section_contexts[i].header->bktr_superblock;
                } else {
                    ctx->section_contexts[i].type = ROMFS;
                    ctx->section_contexts[i].romfs_ctx.superblock = &ctx->section_contexts[i].header->romfs_superblock;
                }
            } else if (ctx->section_contexts[i].header->partition_type == PARTITION_ROMFS && ctx->section_contexts[i].header->fs_type == FS_TYPE_PFS0 && (ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA)) {
                ctx->section_contexts[i].type = NCA0_ROMFS;
                ctx->section_contexts[i].nca0_romfs_ctx.superblock = &ctx->section_contexts[i].header->nca0_romfs_superblock;
            } else {
                ctx->section_contexts[i].type = INVALID;
            }

            uint64_t ofs = ctx->section_contexts[i].offset >> 4;

            for (unsigned int j = 0; j < 0x8; j++) {
                ctx->section_contexts[i].ctr[j] = ctx->section_contexts[i].header->section_ctr[0x8-j-1];
                ctx->section_contexts[i].ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
                ofs >>= 8;
            }

            ctx->section_contexts[i].sector_num = 0;
            ctx->section_contexts[i].sector_ofs = 0;

            if (ctx->section_contexts[i].crypt_type == CRYPT_NONE) {
                ctx->section_contexts[i].is_decrypted = 1;
            }

            if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_contentkey) {
                ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.cli_contentkey, 16, AES_MODE_CTR);
            } else {
                if (ctx->has_rights_id) {
                    if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.dec_cli_titlekey, 16, AES_MODE_CTR);
                    } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
                        titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
                        ctx->section_contexts[i].aes = new_aes_ctx(entry->dec_titlekey, 16, AES_MODE_CTR);
                    } else {
                        if (i == 0) {
                            cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString("Unable to match rights id to titlekey. Update title.keys?"));
                        }
                        unsigned char fallback[0x10] = {0};
                        ctx->section_contexts[i].aes = new_aes_ctx(fallback, 16, AES_MODE_CTR);
                    }
                } else {
                    if (ctx->section_contexts[i].crypt_type == CRYPT_CTR || ctx->section_contexts[i].crypt_type == CRYPT_BKTR) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys[2], 16, AES_MODE_CTR);
                    } else if (ctx->section_contexts[i].crypt_type == CRYPT_XTS || ctx->section_contexts[i].crypt_type == CRYPT_NCA0) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys, 32, AES_MODE_XTS);
                        ctx->section_contexts[i].sector_size = 0x200ULL;
                    }

                    if (ctx->section_contexts[i].sector_size) {
                        ctx->section_contexts[i].sector_mask = ctx->section_contexts[i].sector_size - 1ULL;
                    }
                }
            }

            if (ctx->tool_ctx->action & ACTION_VERIFY) {
                cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Verifying section {}.", i).data()));
            }

            switch (ctx->section_contexts[i].type) {
                case PFS0:
                    nca_process_pfs0_section(&ctx->section_contexts[i], Env);

                    /* Verify NPDM sig now, if we can... */
                    if (ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                        ctx->npdm = ctx->section_contexts[i].pfs0_ctx.npdm;
                        if (rsa2048_pss_verify(&ctx->header.magic, 0x200, ctx->header.npdm_key_sig, npdm_get_acid(ctx->npdm)->modulus)) {
                            ctx->npdm_sig_validity = VALIDITY_VALID;
                        } else {
                            ctx->npdm_sig_validity = VALIDITY_INVALID;
                        }
                    }
                    break;
                case ROMFS:
                    nca_process_ivfc_section(&ctx->section_contexts[i], Env);
                    break;
                case NCA0_ROMFS:
                    nca_process_nca0_romfs_section(&ctx->section_contexts[i], Env);
                    break;
                case BKTR:
                    nca_process_bktr_section(&ctx->section_contexts[i], Env);
                    break;
                case INVALID:
                default:
                    break;
            }
        }
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        nca_print(ctx);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        nca_save(ctx, Env);
    }
}

/* Decrypt NCA header. */
int nca_decrypt_header(nca_ctx_t *ctx, Napi::Env Env) {
    fseeko64(ctx->file, 0, SEEK_SET);

    size_t read_size = fread(&ctx->header, 1, 0xC00, ctx->file);

    if (read_size != 0xC00 && read_size != 0xA00) {
        return 0;
    }

    /* Try to support decrypted NCA headers. */
    if (ctx->header.magic == MAGIC_NCA3 || ctx->header.magic == MAGIC_NCA2) {
        if (ctx->header._0x340[0] == 0 && !memcmp(ctx->header._0x340, ctx->header._0x340 + 1, 0xBF)) {
            ctx->is_decrypted = 1;

            if (ctx->header.magic == MAGIC_NCA3) {
                ctx->format_version = NCAVERSION_NCA3;
            } else {
                ctx->format_version = NCAVERSION_NCA2;
            }

            return 1;
        }
    }

    ctx->is_decrypted = 0;

    nca_header_t dec_header;

    aes_ctx_t *hdr_aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.header_key, 32, AES_MODE_XTS);
    aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0x400, 0, 0x200);

    if (read_size == 0xA00 && dec_header.magic != MAGIC_NCA0) {
        return 0;
    }

    if (dec_header.magic == MAGIC_NCA3) {
        ctx->format_version = NCAVERSION_NCA3;
        aes_xts_decrypt(hdr_aes_ctx, &dec_header, &ctx->header, 0xC00, 0, 0x200);
        ctx->header = dec_header;
    } else if (dec_header.magic == MAGIC_NCA2) {
        ctx->format_version = NCAVERSION_NCA2;

        for (unsigned int i = 0; i < 4; i++) {
            if (dec_header.fs_headers[i]._0x148[0] != 0 || memcmp(dec_header.fs_headers[i]._0x148, dec_header.fs_headers[i]._0x148 + 1, 0xB7)) {
                aes_xts_decrypt(hdr_aes_ctx, &dec_header.fs_headers[i], &ctx->header.fs_headers[i], 0x200, 0, 0x200);
            } else {
                memset(&dec_header.fs_headers[i], 0, sizeof(nca_fs_header_t));
            }
        }

        ctx->header = dec_header;
    } else if (dec_header.magic == MAGIC_NCA0) {
        memset(ctx->decrypted_keys, 0, 0x40);
        unsigned char out_keydata[0x100];
        size_t out_len = 0;

        if (rsa2048_oaep_decrypt_verify(out_keydata, sizeof(out_keydata), (const unsigned char *)dec_header.encrypted_keys, pki_get_beta_nca0_modulus(), pki_get_beta_nca0_exponent(), 0x100, pki_get_beta_nca0_label_hash(), &out_len)) {
            if (out_len >= 0x20) {
                memcpy(ctx->decrypted_keys, out_keydata, 0x20);
                ctx->format_version = NCAVERSION_NCA0_BETA;
            }
        } else {
            unsigned char calc_hash[0x20];
            static const unsigned char expected_hash[0x20] = {0x9A, 0xBB, 0xD2, 0x11, 0x86, 0x00, 0x21, 0x9D, 0x7A, 0xDC, 0x5B, 0x43, 0x95, 0xF8, 0x4E, 0xFD, 0xFF, 0x6B, 0x25, 0xEF, 0x9F, 0x96, 0x85, 0x28, 0x18, 0x9E, 0x76, 0xB0, 0x92, 0xF0, 0x6A, 0xCB};
            sha256_hash_buffer(calc_hash, dec_header.encrypted_keys, 0x20);
            if (memcmp(calc_hash, expected_hash, sizeof(calc_hash)) == 0) {
                ctx->format_version = NCAVERSION_NCA0;
                memcpy(ctx->decrypted_keys, dec_header.encrypted_keys, 0x40);
            } else {
                ctx->format_version = NCAVERSION_NCA0;
                aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.key_area_keys[ctx->crypto_type][dec_header.kaek_ind], 16, AES_MODE_ECB);
                aes_decrypt(aes_ctx, ctx->decrypted_keys, dec_header.encrypted_keys, 0x20);
                free_aes_ctx(aes_ctx);
            }
        }

        if (ctx->format_version != NCAVERSION_UNKNOWN) {
            memset(dec_header.fs_headers, 0, sizeof(dec_header.fs_headers));
            aes_ctx_t *aes_ctx = new_aes_ctx(ctx->decrypted_keys, 32, AES_MODE_XTS);
            for (unsigned int i = 0; i < 4; i++) {
                if (dec_header.section_entries[i].media_start_offset) { /* Section exists. */
                    uint64_t offset = media_to_real(dec_header.section_entries[i].media_start_offset);
                    fseeko64(ctx->tool_ctx->file, offset, SEEK_SET);
                    if (fread(&dec_header.fs_headers[i], sizeof(dec_header.fs_headers[i]), 1, ctx->tool_ctx->file) != 1) {
                        cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Failed to read NCA0 FS header at offset {}.", offset).data()));

                        return 0;
                    }
                    aes_xts_decrypt(aes_ctx, &dec_header.fs_headers[i], &dec_header.fs_headers[i], sizeof(dec_header.fs_headers[i]), (offset - 0x400ULL) >> 9ULL, 0x200);
                }
            }
            free_aes_ctx(aes_ctx);
            ctx->header = dec_header;
        }
    }

    free_aes_ctx(hdr_aes_ctx);
    return ctx->format_version != NCAVERSION_UNKNOWN;
}

/* Decrypt key area. */
void nca_decrypt_key_area(nca_ctx_t *ctx) {
    if (ctx->format_version == NCAVERSION_NCA0_BETA || ctx->format_version == NCAVERSION_NCA0) return;
    aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.key_area_keys[ctx->crypto_type][ctx->header.kaek_ind], 16, AES_MODE_ECB);
    aes_decrypt(aes_ctx, ctx->decrypted_keys, ctx->header.encrypted_keys, 0x40);
    free_aes_ctx(aes_ctx);
}

static const char *nca_get_distribution_type(nca_ctx_t *ctx) {
    switch (ctx->header.distribution) {
        case 0:
            return "Download";
        case 1:
            return "Gamecard";
        default:
            return "Unknown";
    }
}

static const char *nca_get_content_type(nca_ctx_t *ctx) {
    switch (ctx->header.content_type) {
        case 0:
            return "Program";
        case 1:
            return "Meta";
        case 2:
            return "Control";
        case 3:
            return "Manual";
        case 4:
            return "Data";
        case 5:
            return "PublicData";
        default:
            return "Unknown";
    }
}

static const char *nca_get_encryption_type(nca_ctx_t *ctx) {
    if (ctx->has_rights_id) {
        return "Titlekey crypto";
    } else {
        return "Standard crypto";
    }
}

static const char *nca_get_section_type(nca_section_ctx_t *meta) {
    switch (meta->type) {
        case PFS0: {
            if (meta->pfs0_ctx.is_exefs) return "ExeFS";
            return "PFS0";
        }
        case NCA0_ROMFS:     return "NCA0 RomFS";
        case ROMFS:     return "RomFS";
        case BKTR:      return "Patch RomFS";
        case INVALID:
        default:
            return "Unknown/Invalid";
    }
}

static void nca_print_sections(nca_ctx_t *ctx, cJSON *archive_details) {
    cJSON *sections = cJSON_CreateArray();
    cJSON_AddItemToObject(archive_details, "sections", sections);

    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            cJSON *section = cJSON_CreateObject();
            cJSON_AddItemToArray(sections, section);

            cJSON_AddItemToObject(section, "number", cJSON_CreateString(fmt::format("{}", i).data()));
            cJSON_AddItemToObject(section, "offset", cJSON_CreateString(fmt::format("0x{:012X}", ctx->section_contexts[i].offset).data()));
            cJSON_AddItemToObject(section, "size", cJSON_CreateString(fmt::format("0x{:012X}", ctx->section_contexts[i].size).data()));
            cJSON_AddItemToObject(section, "partitionType", cJSON_CreateString(nca_get_section_type(&ctx->section_contexts[i])));

            if (!(ctx->format_version == NCAVERSION_NCA0 || ctx->format_version == NCAVERSION_NCA0_BETA)) {
                nca_update_ctr(ctx->section_contexts[i].ctr, ctx->section_contexts[i].offset);

                cJSON_AddItemToObject(section, "CTR", cJSON_CreateString(fmt::format("0x{:012X}", ctx->section_contexts[i].size).data()));
                cJSON_AddItemToObject(section, "CTR", cJSON_CreateString(memdump_return(ctx->section_contexts[i].ctr, 16)));
            }

            switch (ctx->section_contexts[i].type) {
                case PFS0: {
                    nca_print_pfs0_section(&ctx->section_contexts[i], section);
                    break;
                }
                case ROMFS: {
                    nca_print_ivfc_section(&ctx->section_contexts[i], section);
                    break;
                }
                case NCA0_ROMFS: {
                    nca_print_nca0_romfs_section(&ctx->section_contexts[i], section);
                    break;
                }
                case BKTR: {
                    nca_print_bktr_section(&ctx->section_contexts[i], section);
                    break;
                }
                case INVALID:
                default: {
                    cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString(fmt::format("Unknown or invalid superblock when reading NCA section {}.", i).data()));
                }
            }
        }
    }
}

/* Print out information about the NCA. */
void nca_print(nca_ctx_t *ctx) {
    // Create an object to hold the archive details.
    cJSON *archive_details = cJSON_CreateObject();
    cJSON_AddItemToObject(ctx->tool_ctx->output, "archive", archive_details);
    cJSON_AddItemToObject(archive_details, "magic", cJSON_CreateString(return_magic(ctx->header.magic)));

    cJSON *fixedKeyIndex = cJSON_CreateObject();
    cJSON_AddItemToObject(archive_details, "fixedKeyIndex", fixedKeyIndex);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->header.fixed_key_generation < sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli) / sizeof(ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_moduli[0])) {
            cJSON_AddItemToObject(fixedKeyIndex, "status", cJSON_CreateString("good"));
        } else {
            cJSON_AddItemToObject(fixedKeyIndex, "status", cJSON_CreateString("fail"));
        }
    } else {
        cJSON_AddItemToObject(fixedKeyIndex, "status", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(fixedKeyIndex, "index", cJSON_CreateString(fmt::format("0x{}", ctx->header.fixed_key_generation).data()));

    cJSON *fixedKeySignature = cJSON_CreateObject();
    cJSON_AddItemToObject(archive_details, "fixedKeySignature", fixedKeySignature);

    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->fixed_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->fixed_sig_validity == VALIDITY_VALID) {
            cJSON_AddItemToObject(fixedKeySignature, "status", cJSON_CreateString("good"));
        } else {
            cJSON_AddItemToObject(fixedKeySignature, "status", cJSON_CreateString("fail"));
        }
    } else {
        cJSON_AddItemToObject(fixedKeySignature, "status", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(fixedKeySignature, "signature", cJSON_CreateString(memdump_return(ctx->header.fixed_key_sig, 0x100)));

    cJSON *npdmSignature = cJSON_CreateObject();
    cJSON_AddItemToObject(archive_details, "npdmSignature", npdmSignature);

    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->npdm_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->npdm_sig_validity == VALIDITY_VALID) {
            cJSON_AddItemToObject(npdmSignature, "status", cJSON_CreateString("good"));
        } else {
            cJSON_AddItemToObject(npdmSignature, "status", cJSON_CreateString("fail"));
        }
    } else {
        cJSON_AddItemToObject(npdmSignature, "status", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(npdmSignature, "signature", cJSON_CreateString(memdump_return(ctx->header.npdm_key_sig, 0x100)));

    cJSON_AddItemToObject(archive_details, "contentSize", cJSON_CreateString(fmt::format("0x{:012X}", ctx->header.nca_size).data()));
    cJSON_AddItemToObject(archive_details, "titleId", cJSON_CreateString(fmt::format("{:016X}", ctx->header.title_id).data()));

    cJSON *sdk = cJSON_CreateObject();
    cJSON_AddItemToObject(archive_details, "sdk", sdk);

    cJSON_AddItemToObject(sdk, "version", cJSON_CreateString(fmt::format("{}.{}.{}.{}", ctx->header.sdk_major, ctx->header.sdk_minor, ctx->header.sdk_micro, ctx->header.sdk_revision).data()));
    cJSON_AddItemToObject(sdk, "major", cJSON_CreateString(fmt::format("{}", ctx->header.sdk_major).data()));
    cJSON_AddItemToObject(sdk, "minor", cJSON_CreateString(fmt::format("{}", ctx->header.sdk_minor).data()));
    cJSON_AddItemToObject(sdk, "micro", cJSON_CreateString(fmt::format("{}", ctx->header.sdk_micro).data()));
    cJSON_AddItemToObject(sdk, "revision", cJSON_CreateString(fmt::format("{}", ctx->header.sdk_revision).data()));

    cJSON_AddItemToObject(archive_details, "distributionType", cJSON_CreateString(nca_get_distribution_type(ctx)));
    cJSON_AddItemToObject(archive_details, "contentType", cJSON_CreateString(nca_get_content_type(ctx)));
    cJSON_AddItemToObject(archive_details, "encryptionType", cJSON_CreateString(nca_get_encryption_type(ctx)));

    cJSON *masterKey = cJSON_CreateObject();
    cJSON_AddItemToObject(archive_details, "masterKey", masterKey);

    cJSON_AddItemToObject(masterKey, "revision", cJSON_CreateString(fmt::format("{:#X}", ctx->crypto_type).data()));
    cJSON_AddItemToObject(masterKey, "firmware", cJSON_CreateString(get_key_revision_summary(ctx->crypto_type)));

    if (ctx->has_rights_id) {
        cJSON_AddItemToObject(archive_details, "rightsID", cJSON_CreateString(memdump_return(ctx->header.rights_id, 0x10)));

        cJSON *titleKey = cJSON_CreateObject();
        cJSON_AddItemToObject(archive_details, "titleKey", titleKey);

        if (ctx->is_cli_target && ctx->tool_ctx->settings.has_cli_titlekey) {
            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                cJSON_AddItemToObject(titleKey, "fromCLI", cJSON_CreateTrue());
                cJSON_AddItemToObject(titleKey, "encrypted", cJSON_CreateString(memdump_return(ctx->tool_ctx->settings.cli_titlekey, 0x10)));
                cJSON_AddItemToObject(titleKey, "decrypted", cJSON_CreateString(memdump_return(ctx->tool_ctx->settings.dec_cli_titlekey, 0x10)));
            }
        } else if (settings_has_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id)) {
            titlekey_entry_t *entry = settings_get_titlekey(&ctx->tool_ctx->settings, ctx->header.rights_id);
            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                cJSON_AddItemToObject(titleKey, "fromCLI", cJSON_CreateFalse());
                cJSON_AddItemToObject(titleKey, "encrypted", cJSON_CreateString(memdump_return(entry->titlekey, 0x10)));
                cJSON_AddItemToObject(titleKey, "decrypted", cJSON_CreateString(memdump_return(entry->dec_titlekey, 0x10)));
            }
        } else {
            cJSON_ReplaceItemInObjectCaseSensitive(archive_details, "titleKey", cJSON_CreateString("unknown"));
        }
    } else {
        cJSON *keyArea = cJSON_CreateObject();
        cJSON_AddItemToObject(archive_details, "keyArea", keyArea);
        cJSON_AddItemToObject(keyArea, "encryptionKey", cJSON_CreateString(fmt::format("{}", ctx->header.kaek_ind).data()));

        cJSON *key0 = cJSON_CreateObject();
        cJSON *key1 = cJSON_CreateObject();
        cJSON_AddItemToObject(keyArea, "key0", key0);
        cJSON_AddItemToObject(keyArea, "key1", key1);

        if (ctx->format_version == NCAVERSION_NCA0_BETA) {
            cJSON_AddItemToObject(keyArea, "RSAOAEP", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys, 0x100)));

            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                cJSON_AddItemToObject(key0, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[0], 0x10)));
                cJSON_AddItemToObject(key1, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[1], 0x10)));
            }
        } else if (ctx->format_version == NCAVERSION_NCA0) {
            cJSON_AddItemToObject(key0, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[0], 0x10)));
            cJSON_AddItemToObject(key1, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[1], 0x10)));

            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                cJSON_AddItemToObject(key0, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[0], 0x10)));
                cJSON_AddItemToObject(key1, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[1], 0x10)));
            }
        } else {
            cJSON *key2 = cJSON_CreateObject();
            cJSON *key3 = cJSON_CreateObject();
            cJSON_AddItemToObject(keyArea, "key2", key2);
            cJSON_AddItemToObject(keyArea, "key3", key3);

            cJSON_AddItemToObject(key0, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[0], 0x10)));
            cJSON_AddItemToObject(key1, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[1], 0x10)));
            cJSON_AddItemToObject(key2, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[2], 0x10)));
            cJSON_AddItemToObject(key3, "encrypted", cJSON_CreateString(memdump_return(ctx->header.encrypted_keys[3], 0x10)));

            if (!ctx->tool_ctx->settings.suppress_keydata_output) {
                cJSON_AddItemToObject(key0, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[0], 0x10)));
                cJSON_AddItemToObject(key1, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[1], 0x10)));
                cJSON_AddItemToObject(key2, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[2], 0x10)));
                cJSON_AddItemToObject(key3, "decrypted", cJSON_CreateString(memdump_return(ctx->decrypted_keys[3], 0x10)));
            }
        }
    }

    if (ctx->npdm) {
        npdm_process(ctx->npdm, ctx->tool_ctx);
    }

    nca_print_sections(ctx, archive_details);
}

static validity_t nca_section_check_external_hash_table(nca_section_ctx_t *ctx, unsigned char *hash_table, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block, Napi::Env Env) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }

    unsigned char cur_hash[0x20];
    uint64_t read_size = block_size;
    unsigned char *block = static_cast<unsigned char *>(malloc(block_size));

    if (block == NULL) {
        throw Napi::Error::New(Env, "Failed to allocate hash block!");
    }

    validity_t result = VALIDITY_VALID;
    unsigned char *cur_hash_table_entry = hash_table;

    for (uint64_t ofs = 0; ofs < data_len; ofs += read_size) {
        nca_section_fseek(ctx, ofs + data_ofs);

        if (ofs + read_size > data_len) {
            /* Last block... */
            memset(block, 0, read_size);
            read_size = data_len - ofs;
        }

        if (nca_section_fread(ctx, block, read_size, Env) != read_size) {
            throw Napi::Error::New(Env, "Failed to read NCA section!");
        }

        sha256_hash_buffer(cur_hash, block, full_block ? block_size : read_size);

        if (memcmp(cur_hash, cur_hash_table_entry, 0x20) != 0) {
            result = VALIDITY_INVALID;
            break;
        }

        cur_hash_table_entry += 0x20;
    }

    free(block);

    return result;
}

static validity_t nca_section_check_hash_table(nca_section_ctx_t *ctx, uint64_t hash_ofs, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block, Napi::Env Env) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }

    uint64_t hash_table_size = data_len / block_size;

    if (data_len % block_size) {
        hash_table_size++;
    }

    hash_table_size *= 0x20;
    unsigned char *hash_table = static_cast<unsigned char *>(malloc(hash_table_size));

    if (hash_table == NULL) {
        throw Napi::Error::New(Env, "Failed to allocate hash table!");
    }

    nca_section_fseek(ctx, hash_ofs);

    if (nca_section_fread(ctx, hash_table, hash_table_size, Env) != hash_table_size) {
        throw Napi::Error::New(Env, "Failed to read NCA section!");
    }

    validity_t result = nca_section_check_external_hash_table(ctx, hash_table, data_ofs, data_len, block_size, full_block, Env);

    free(hash_table);

    return result;
}

static void nca_save_pfs0_file(nca_section_ctx_t *ctx, uint32_t i, filepath_t *dirpath, Napi::Env Env) {
    if (i >= ctx->pfs0_ctx.header->num_files) {
        throw Napi::Error::New(Env, fmt::format("Could not save PFS0 file {}.", i).data());
    }

    pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);

    if (cur_file->size >= ctx->size) {
        throw Napi::Error::New(Env, fmt::format("File {} is too big in PFS0 section {}.", i, ctx->section_num).data());
    }

    if (strlen(pfs0_get_file_name(ctx->pfs0_ctx.header, i)) >= MAX_PATH - strlen(dirpath->char_path) - 1) {
        throw Napi::Error::New(Env, fmt::format("File {} filename is too long in PFS0 section {}.", i, ctx->section_num).data());
    }

    filepath_t filepath;
    filepath_copy(&filepath, dirpath);
    filepath_append(&filepath, "%s", pfs0_get_file_name(ctx->pfs0_ctx.header, i));

    cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Saving {} to {}.", pfs0_get_file_name(ctx->pfs0_ctx.header, i), filepath.char_path).data()));

    uint64_t ofs = ctx->pfs0_ctx.superblock->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset;
    nca_save_section_file(ctx, ofs, cur_file->size, &filepath, Env);
}

void nca_process_pfs0_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    pfs0_superblock_t *sb = ctx->pfs0_ctx.superblock;
    ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->master_hash, sb->hash_table_offset, sb->hash_table_size, sb->hash_table_size, 0, Env);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        /* Verify actual PFS0... */
        ctx->pfs0_ctx.hash_table_validity = nca_section_check_hash_table(ctx, sb->hash_table_offset, sb->pfs0_offset, sb->pfs0_size, sb->block_size, 0, Env);
    }

    if (ctx->superblock_hash_validity != VALIDITY_VALID) {
        return;
    }

    /* Read *just* safe amount. */
    pfs0_header_t raw_header;
    nca_section_fseek(ctx, sb->pfs0_offset);

    if (nca_section_fread(ctx, &raw_header, sizeof(raw_header), Env) != sizeof(raw_header)) {
        throw Napi::Error::New(Env, "Failed to read PFS0 header.");
    }

    uint64_t header_size = pfs0_get_header_size(&raw_header);
    ctx->pfs0_ctx.header = static_cast<pfs0_header_t *>(malloc(header_size));

    if (ctx->pfs0_ctx.header == NULL) {
        throw Napi::Error::New(Env, "Failed to get PFS0 header size.");
    }

    nca_section_fseek(ctx, sb->pfs0_offset);

    if (nca_section_fread(ctx, ctx->pfs0_ctx.header, header_size, Env) != header_size) {
        throw Napi::Error::New(Env, "Failed to read PFS0 header.");
    }

    for (unsigned int i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
        pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);

        if (strcmp(pfs0_get_file_name(ctx->pfs0_ctx.header, i), "main.npdm") == 0) {
            /* We might have found the exefs... */
            if (cur_file->size >= sb->pfs0_size) {
                throw Napi::Error::New(Env, "NPDM too big.");
            }

            ctx->pfs0_ctx.npdm = static_cast<npdm_t *>(malloc(cur_file->size));

            if (ctx->pfs0_ctx.npdm == NULL) {
                throw Napi::Error::New(Env, "Failed to allocate NPDM.");
            }

            nca_section_fseek(ctx, sb->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset);

            if (nca_section_fread(ctx, ctx->pfs0_ctx.npdm, cur_file->size, Env) != cur_file->size) {
                throw Napi::Error::New(Env, "Failed to read NPDM.");
            }

            if (ctx->pfs0_ctx.npdm->magic == MAGIC_META) {
                ctx->pfs0_ctx.is_exefs = 1;
            }
        }
    }
}

void nca_process_ivfc_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    romfs_superblock_t *sb = ctx->romfs_ctx.superblock;

    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        /* Load in the current level's header data. */
        ivfc_level_ctx_t *cur_level = &ctx->romfs_ctx.ivfc_levels[i];
        cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
        cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
        cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

        if (i != 0) {
            /* Hash table is previous level's data. */
            cur_level->hash_offset = ctx->romfs_ctx.ivfc_levels[i-1].data_offset;
        } else {
            /* Hash table is the superblock hash. Always check the superblock hash. */
            ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1, Env);
            cur_level->hash_validity = ctx->superblock_hash_validity;
        }

        if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
            /* Actually check the table. */
            cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Verifying IVFC Level", i).data()));

            cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1, Env);
        }
    }

    ctx->romfs_ctx.romfs_offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
    nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset);

    if (nca_section_fread(ctx, &ctx->romfs_ctx.header, sizeof(romfs_hdr_t), Env) != sizeof(romfs_hdr_t)) {
        cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString("Failed to read RomFS header"));
    }

    if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
        /* Pre-load the file/data entry caches. */
        ctx->romfs_ctx.directories = static_cast<romfs_direntry_t *>(calloc(1, ctx->romfs_ctx.header.dir_meta_table_size));

        if (ctx->romfs_ctx.directories == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate RomFS directory cache.");
        }

        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.dir_meta_table_offset);

        if (nca_section_fread(ctx, ctx->romfs_ctx.directories, ctx->romfs_ctx.header.dir_meta_table_size, Env) != ctx->romfs_ctx.header.dir_meta_table_size) {
            throw Napi::Error::New(Env, "Failed to read RomFS directory cache.");
        }

        ctx->romfs_ctx.files = static_cast<romfs_fentry_t *>(calloc(1, ctx->romfs_ctx.header.file_meta_table_size));

        if (ctx->romfs_ctx.files == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate RomFS file cache.");
        }

        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.file_meta_table_offset);

        if (nca_section_fread(ctx, ctx->romfs_ctx.files, ctx->romfs_ctx.header.file_meta_table_size, Env) != ctx->romfs_ctx.header.file_meta_table_size) {
            throw Napi::Error::New(Env, "Failed to read RomFS file cache.");
        }
    }
}

void nca_process_nca0_romfs_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    nca0_romfs_superblock_t *sb = ctx->nca0_romfs_ctx.superblock;
    ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->master_hash, sb->hash_table_offset, sb->hash_table_size, sb->hash_table_size, 0, Env);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        /* Verify actual ROMFS... */
        ctx->nca0_romfs_ctx.hash_table_validity = nca_section_check_hash_table(ctx, sb->hash_table_offset, sb->romfs_offset, sb->romfs_size, sb->block_size, 0, Env);
    }

    if (ctx->superblock_hash_validity != VALIDITY_VALID) {
        return;
    }

    ctx->nca0_romfs_ctx.romfs_offset = sb->romfs_offset;
    nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset);

    if (nca_section_fread(ctx, &ctx->nca0_romfs_ctx.header, sizeof(nca0_romfs_hdr_t), Env) != sizeof(nca0_romfs_hdr_t)) {
        cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString("Failed to read NCA0 RomFS header."));
    }

    if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->nca0_romfs_ctx.header.header_size == NCA0_ROMFS_HEADER_SIZE) {
        /* Pre-load the file/data entry caches. */
        ctx->nca0_romfs_ctx.directories = static_cast<romfs_direntry_t *>(calloc(1, ctx->nca0_romfs_ctx.header.dir_meta_table_size));

        if (ctx->nca0_romfs_ctx.directories == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate NCA0 RomFS directory cache.");
        }

        nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.dir_meta_table_offset);

        if (nca_section_fread(ctx, ctx->nca0_romfs_ctx.directories, ctx->nca0_romfs_ctx.header.dir_meta_table_size, Env) != ctx->nca0_romfs_ctx.header.dir_meta_table_size) {
            throw Napi::Error::New(Env, "Failed to read NCA0 RomFS directory cache.");
        }

        ctx->nca0_romfs_ctx.files = static_cast<romfs_fentry_t *>(calloc(1, ctx->nca0_romfs_ctx.header.file_meta_table_size));

        if (ctx->nca0_romfs_ctx.files == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate NCA0 RomFS file cache.");
        }

        nca_section_fseek(ctx, ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.file_meta_table_offset);

        if (nca_section_fread(ctx, ctx->nca0_romfs_ctx.files, ctx->nca0_romfs_ctx.header.file_meta_table_size, Env) != ctx->nca0_romfs_ctx.header.file_meta_table_size) {
            throw Napi::Error::New(Env, "Failed to read NCA0 RomFS file cache.");
        }
    }
}

void nca_process_bktr_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    bktr_superblock_t *sb = ctx->bktr_ctx.superblock;

    /* Validate magics. */
    if (sb->relocation_header.magic == MAGIC_BKTR && sb->subsection_header.magic == MAGIC_BKTR) {
        if (sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset || sb->subsection_header.offset + sb->subsection_header.size != ctx->size) {

            throw Napi::Error::New(Env, "Invalid BKTR layout.");
        }

        /* Allocate space for an extra (fake) relocation entry, to simplify our logic. */
        void *relocs = calloc(1, sb->relocation_header.size + (0x3FF0 / sizeof(uint64_t)) * sizeof(bktr_relocation_entry_t));

        if (relocs == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate relocation header.");
        }

        /* Allocate space for an extra (fake) subsection entry, to simplify our logic. */
        void *subs = calloc(1, sb->subsection_header.size + (0x3FF0 / sizeof(uint64_t)) * sizeof(bktr_subsection_entry_t) + sizeof(bktr_subsection_entry_t));

        if (subs == NULL) {
            throw Napi::Error::New(Env, "Failed to allocate subsection header.");
        }

        nca_section_fseek(ctx, sb->relocation_header.offset);

        if (nca_section_fread(ctx, relocs, sb->relocation_header.size, Env) != sb->relocation_header.size) {
            throw Napi::Error::New(Env, "Failed to read relocation header.");
        }

        nca_section_fseek(ctx, sb->subsection_header.offset);

        if (nca_section_fread(ctx, subs, sb->subsection_header.size, Env) != sb->subsection_header.size) {
            throw Napi::Error::New(Env, "Failed to read subsection header.");
        }

        /* NOTE: Setting these variables changes the way fseek/fread work! */
        ctx->bktr_ctx.relocation_block = static_cast<bktr_relocation_block_t *>(relocs);
        ctx->bktr_ctx.subsection_block = static_cast<bktr_subsection_block_t *>(subs);

        if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {
            free(relocs);
            free(subs);
            ctx->bktr_ctx.relocation_block = NULL;
            ctx->bktr_ctx.subsection_block = NULL;
            ctx->superblock_hash_validity = VALIDITY_INVALID;
            return;
        }

        /* This simplifies logic greatly... */
        for (unsigned int i = ctx->bktr_ctx.relocation_block->num_buckets - 1; i > 0; i--) {
            memcpy(bktr_get_relocation_bucket(ctx->bktr_ctx.relocation_block, i), &ctx->bktr_ctx.relocation_block->buckets[i], sizeof(bktr_relocation_bucket_t));
        }

        for (unsigned int i = 0; i + 1 < ctx->bktr_ctx.relocation_block->num_buckets; i++) {
            bktr_relocation_bucket_t *cur_bucket = bktr_get_relocation_bucket(ctx->bktr_ctx.relocation_block, i);
            cur_bucket->entries[cur_bucket->num_entries].virt_offset = ctx->bktr_ctx.relocation_block->bucket_virtual_offsets[i + 1];
        }

        for (unsigned int i = ctx->bktr_ctx.subsection_block->num_buckets - 1; i > 0; i--) {
            memcpy(bktr_get_subsection_bucket(ctx->bktr_ctx.subsection_block, i), &ctx->bktr_ctx.subsection_block->buckets[i], sizeof(bktr_subsection_bucket_t));
        }

        for (unsigned int i = 0; i + 1 < ctx->bktr_ctx.subsection_block->num_buckets; i++) {
            bktr_subsection_bucket_t *cur_bucket = bktr_get_subsection_bucket(ctx->bktr_ctx.subsection_block, i);
            bktr_subsection_bucket_t *next_bucket = bktr_get_subsection_bucket(ctx->bktr_ctx.subsection_block, i+1);
            cur_bucket->entries[cur_bucket->num_entries].offset = next_bucket->entries[0].offset;
            cur_bucket->entries[cur_bucket->num_entries].ctr_val = next_bucket->entries[0].ctr_val;
        }

        bktr_relocation_bucket_t *last_reloc_bucket = bktr_get_relocation_bucket(ctx->bktr_ctx.relocation_block, ctx->bktr_ctx.relocation_block->num_buckets - 1);
        bktr_subsection_bucket_t *last_subsec_bucket = bktr_get_subsection_bucket(ctx->bktr_ctx.subsection_block, ctx->bktr_ctx.subsection_block->num_buckets - 1);
        last_reloc_bucket->entries[last_reloc_bucket->num_entries].virt_offset = ctx->bktr_ctx.relocation_block->total_size;
        last_subsec_bucket->entries[last_subsec_bucket->num_entries].offset = sb->relocation_header.offset;
        last_subsec_bucket->entries[last_subsec_bucket->num_entries].ctr_val = ctx->header->section_ctr_low;
        last_subsec_bucket->entries[last_subsec_bucket->num_entries + 1].offset = ctx->size;
        last_subsec_bucket->entries[last_subsec_bucket->num_entries + 1].ctr_val = 0;

        /* Now parse out the romfs stuff. */
        for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
            /* Load in the current level's header data. */
            ivfc_level_ctx_t *cur_level = &ctx->bktr_ctx.ivfc_levels[i];
            cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
            cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
            cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

            if (i != 0) {
                /* Hash table is previous level's data. */
                cur_level->hash_offset = ctx->bktr_ctx.ivfc_levels[i-1].data_offset;
            } else if (ctx->tool_ctx->base_file != NULL) {
                /* Hash table is the superblock hash. Always check the superblock hash. */
                ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1, Env);
                cur_level->hash_validity = ctx->superblock_hash_validity;
            }

            if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
                /* Actually check the table. */
                cJSON_AddItemToArray(ctx->tool_ctx->log, cJSON_CreateString(fmt::format("Verifying IVFC Level %{}", i).data()));

                cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1, Env);
            }
        }

        ctx->bktr_ctx.romfs_offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;

        if (ctx->tool_ctx->base_file != NULL) {
            nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset);

            if (nca_section_fread(ctx, &ctx->bktr_ctx.header, sizeof(romfs_hdr_t), Env) != sizeof(romfs_hdr_t)) {
                cJSON_AddItemToArray(ctx->tool_ctx->warnings, cJSON_CreateString("Failed to read BKTR Virtual RomFS header."));
            }

            if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
                /* Pre-load the file/data entry caches. */
                ctx->bktr_ctx.directories = static_cast<romfs_direntry_t *>(calloc(1, ctx->bktr_ctx.header.dir_meta_table_size));

                if (ctx->bktr_ctx.directories == NULL) {
                    throw Napi::Error::New(Env, "Failed to allocate RomFS directory cache.");
                }

                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.dir_meta_table_offset);

                if (nca_section_fread(ctx, ctx->bktr_ctx.directories, ctx->bktr_ctx.header.dir_meta_table_size, Env) != ctx->bktr_ctx.header.dir_meta_table_size) {
                    throw Napi::Error::New(Env, "Failed to read RomFS directory cache.");
                }

                ctx->bktr_ctx.files = static_cast<romfs_fentry_t *>(calloc(1, ctx->bktr_ctx.header.file_meta_table_size));

                if (ctx->bktr_ctx.files == NULL) {
                    throw Napi::Error::New(Env, "Failed to allocate RomFS file cache.");
                }

                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.file_meta_table_offset);

                if (nca_section_fread(ctx, ctx->bktr_ctx.files, ctx->bktr_ctx.header.file_meta_table_size, Env) != ctx->bktr_ctx.header.file_meta_table_size) {
                    throw Napi::Error::New(Env, "Failed to read RomFS file cache.");
                }
            }
        }
    }
}

void nca_print_pfs0_section(nca_section_ctx_t *ctx, cJSON *section) {
    cJSON_AddItemToObject(section, "PFS0Offset", cJSON_CreateString(fmt::format("{:012X}", ctx->pfs0_ctx.superblock->pfs0_offset).data()));
    cJSON_AddItemToObject(section, "PFS0Size", cJSON_CreateString(fmt::format("{:012X}", ctx->pfs0_ctx.superblock->pfs0_size).data()));

    cJSON *superblock = cJSON_CreateObject();
    cJSON_AddItemToObject(section, "superblock", superblock);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            cJSON_AddItemToObject(superblock, "status", cJSON_CreateString("good"));
        } else {
            cJSON_AddItemToObject(superblock, "status", cJSON_CreateString("fail"));
        }
    } else {
        cJSON_AddItemToObject(superblock, "status", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(superblock, "hash", cJSON_CreateString(memdump_return(ctx->pfs0_ctx.superblock->master_hash, 0x20)));

    cJSON *hashTable = cJSON_CreateObject();
    cJSON_AddItemToObject(section, "hashTable", hashTable);
    cJSON_AddItemToObject(hashTable, "status", cJSON_CreateString(GET_VALIDITY_STR(ctx->pfs0_ctx.hash_table_validity)));
    cJSON_AddItemToObject(hashTable, "offset", cJSON_CreateString(fmt::format("{:012X}", ctx->pfs0_ctx.superblock->hash_table_offset).data()));
    cJSON_AddItemToObject(hashTable, "size", cJSON_CreateString(fmt::format("{:012X}", ctx->pfs0_ctx.superblock->hash_table_size).data()));
    cJSON_AddItemToObject(hashTable, "blockSize", cJSON_CreateString(fmt::format("0x{:X}", ctx->pfs0_ctx.superblock->block_size).data()));
}

void nca_print_ivfc_section(nca_section_ctx_t *ctx, cJSON *section) {
    cJSON *superblock = cJSON_CreateObject();
    cJSON_AddItemToObject(section, "superblock", superblock);

    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            cJSON_AddItemToObject(superblock, "status", cJSON_CreateString("good"));
        } else {
            cJSON_AddItemToObject(superblock, "status", cJSON_CreateString("fail"));
        }
    } else {
        cJSON_AddItemToObject(superblock, "status", cJSON_CreateString(""));
    }

    cJSON_AddItemToObject(superblock, "hash", cJSON_CreateString(memdump_return(ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20)));

    cJSON_AddItemToObject(section, "magic", cJSON_CreateString(return_magic(ctx->romfs_ctx.superblock->ivfc_header.magic)));
    cJSON_AddItemToObject(section, "id", cJSON_CreateString(fmt::format("{:08X}", ctx->romfs_ctx.superblock->ivfc_header.id).data()));

    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (ctx->tool_ctx->action & ACTION_VERIFY) {
            printf("        Level %" PRId32 " (%s):\n", i, GET_VALIDITY_STR(ctx->romfs_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %" PRId32 ":\n", i);
        }
        printf("            Data Offset:            0x%012" PRIx64 "\n", ctx->romfs_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012" PRIx64 "\n", ctx->romfs_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012" PRIx64 "\n", ctx->romfs_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08" PRIx32 "\n", ctx->romfs_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_print_nca0_romfs_section(nca_section_ctx_t *ctx, cJSON *section) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        }
        printf("        Hash Table (%s):\n", GET_VALIDITY_STR(ctx->nca0_romfs_ctx.hash_table_validity));
    } else {
        memdump(stdout, "        Superblock Hash:            ", &ctx->nca0_romfs_ctx.superblock->master_hash, 0x20);
        printf("        Hash Table:\n");
    }

    printf("            Offset:                 %012" PRIx64 "\n", ctx->nca0_romfs_ctx.superblock->hash_table_offset);
    printf("            Size:                   %012" PRIx64 "\n", ctx->nca0_romfs_ctx.superblock->hash_table_size);
    printf("            Block Size:             0x%" PRIx32 "\n", ctx->nca0_romfs_ctx.superblock->block_size);
    printf("        RomFS Offset:               %012" PRIx64 "\n", ctx->nca0_romfs_ctx.superblock->romfs_offset);
    printf("        RomFS Size:                 %012" PRIx64 "\n", ctx->nca0_romfs_ctx.superblock->romfs_size);
}

void nca_print_bktr_section(nca_section_ctx_t *ctx, cJSON *section) {
    if (ctx->bktr_ctx.subsection_block == NULL) {
        printf("        BKTR section seems to be corrupted.\n");
        return;
    }

    int did_verify = (ctx->tool_ctx->action & ACTION_VERIFY) && (ctx->tool_ctx->base_file != NULL);

    if (did_verify ) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        }
    } else {
            memdump(stdout, "        Superblock Hash:            ", &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
    }

    print_magic("        Magic:                      ", ctx->bktr_ctx.superblock->ivfc_header.magic);
    printf("        ID:                         %08" PRIx32 "\n", ctx->bktr_ctx.superblock->ivfc_header.id);

    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (did_verify) {
            printf("        Level %" PRId32 " (%s):\n", i, GET_VALIDITY_STR(ctx->bktr_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %" PRId32 ":\n", i);
        }
        printf("            Data Offset:            0x%012" PRIx64 "\n", ctx->bktr_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012" PRIx64 "\n", ctx->bktr_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012" PRIx64 "\n", ctx->bktr_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08" PRIx32 "\n", ctx->bktr_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_save_section_file(nca_section_ctx_t *ctx, uint64_t ofs, uint64_t total_size, filepath_t *filepath, Napi::Env Env) {
    FILE *f_out = os_fopen(filepath->os_path, OS_MODE_WRITE);

    if (f_out == NULL) {
        fprintf(stderr, "Failed to open %s!\n", filepath->char_path);
        return;
    }

    uint64_t read_size = 0x400000; /* 4 MB buffer. */
    unsigned char *buf = static_cast<unsigned char *>(malloc(read_size));
    if (buf == NULL) {
        fprintf(stderr, "Failed to allocate file-save buffer!\n");
        exit(EXIT_FAILURE);
    }
    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
    uint64_t end_ofs = ofs + total_size;
    while (ofs < end_ofs) {
        nca_section_fseek(ctx, ofs);
        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
        if (nca_section_fread(ctx, buf, read_size, Env) != read_size) {
            fprintf(stderr, "Failed to read file!\n");
            exit(EXIT_FAILURE);
        }
        if (fwrite(buf, 1, read_size, f_out) != read_size) {
            fprintf(stderr, "Failed to write file!\n");
            exit(EXIT_FAILURE);
        }
        ofs += read_size;
    }

    fclose(f_out);

    free(buf);
}

void nca_save_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    /* Save raw section file... */
    uint64_t offset = 0;
    uint64_t size = ctx->size;
    if (!(ctx->tool_ctx->action & ACTION_RAW)) {
        switch (ctx->type) {
            case PFS0:
                offset = ctx->pfs0_ctx.superblock->pfs0_offset;
                size = ctx->pfs0_ctx.superblock->pfs0_size;
                break;
            case ROMFS:
                offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                size = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                break;
            case NCA0_ROMFS:
                offset = ctx->nca0_romfs_ctx.superblock->romfs_offset;
                size = ctx->nca0_romfs_ctx.superblock->romfs_size;
                break;
            case BKTR:
                if (ctx->tool_ctx->base_file != NULL) {
                    offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                    size = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                }
                break;
            case INVALID:
                break;
        }
    } else if (ctx->type == BKTR && ctx->bktr_ctx.subsection_block != NULL && ctx->tool_ctx->base_file != NULL) {
        size = ctx->bktr_ctx.relocation_block->total_size;
    }

    /* Extract to file. */
    filepath_t *secpath = &ctx->tool_ctx->settings.section_paths[ctx->section_num];

    /* Handle overrides. */
    if (ctx->type == PFS0 && ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_path.enabled && ctx->tool_ctx->settings.exefs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.exefs_path.path;
    } else if ((ctx->type == ROMFS || ctx->type == NCA0_ROMFS) && ctx->tool_ctx->settings.romfs_path.enabled && ctx->tool_ctx->settings.romfs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.romfs_path.path;
    }

    if (secpath != NULL && secpath->valid == VALIDITY_VALID) {
        filepath_t appended_path;
        filepath_init(&appended_path);
        filepath_copy(&appended_path, secpath);
        if (ctx->tool_ctx->settings.append_section_types) {
            filepath_set_format(&appended_path, "%s.%s", secpath->char_path, nca_get_section_type_name(ctx->type));
            if (appended_path.valid == VALIDITY_VALID) {
                secpath = &appended_path;
            } else {
                printf("[WARN] Failed to append section type to path\n");
            }
        }

        printf("Saving Section %" PRId32 " to %s.\n", ctx->section_num, secpath->char_path);
        printf("Size: %012" PRIx64 "\n", size);
        nca_save_section_file(ctx, offset, size, secpath, Env);
    }

    switch (ctx->type) {
        case PFS0:
            nca_save_pfs0_section(ctx, Env);
            break;
        case ROMFS:
            nca_save_ivfc_section(ctx, Env);
            break;
        case NCA0_ROMFS:
            nca_save_nca0_romfs_section(ctx, Env);
            break;
        case BKTR:
            if (ctx->tool_ctx->base_file == NULL) {
                fprintf(stderr, "Note: cannot save BKTR section without base romfs.\n");
                break;
            }
            nca_save_bktr_section(ctx, Env);
            break;
        case INVALID:
            break;
    }
}

void nca_save_pfs0_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID && ctx->pfs0_ctx.header->magic == MAGIC_PFS0) {
        /* Extract to directory. */
        filepath_t *dirpath = NULL;
        if (ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_dir_path.enabled) {
            dirpath = &ctx->tool_ctx->settings.exefs_dir_path.path;
        }
        if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
            dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
        }
        if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
            filepath_t appended_path;
            filepath_init(&appended_path);
            filepath_copy(&appended_path, dirpath);
            if (ctx->tool_ctx->settings.append_section_types) {
                filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                if (appended_path.valid == VALIDITY_VALID) {
                    dirpath = &appended_path;
                } else {
                    printf("[WARN] Failed to append section type to path\n");
                }
            }

            os_makedir(dirpath->os_path);
            for (uint32_t i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
                nca_save_pfs0_file(ctx, i, dirpath, Env);
            }
        }
    } else {
        fprintf(stderr, "Error: section %" PRId32 " is corrupted!\n", ctx->section_num);
        return;
    }
}

/* RomFS functions... */
static int nca_is_romfs_file_updated(nca_section_ctx_t *ctx, uint64_t file_offset, uint64_t file_size) {
    /* All files in a Base RomFS are "updated". */
    if (ctx->type == ROMFS) {
        return 1;
    }

    bktr_relocation_entry_t *first_reloc = bktr_get_relocation(ctx->bktr_ctx.relocation_block, file_offset);
    bktr_relocation_entry_t *last_reloc = first_reloc;
    while (last_reloc->virt_offset < file_offset + file_size) {
        last_reloc++;
    }

    for (bktr_relocation_entry_t *cur_reloc = first_reloc; cur_reloc < last_reloc; cur_reloc++) {
        if (cur_reloc->is_patch) {
            return 1;
        }
    }

    return 0;
}

static int nca_visit_romfs_file(nca_section_ctx_t *ctx, uint32_t file_offset, filepath_t *dir_path, Napi::Env Env) {
    filepath_t *cur_path = static_cast<filepath_t *>(calloc(1, sizeof(filepath_t)));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    int found_any_file = 0;

    do {
        romfs_fentry_t *entry;
        if (ctx->type == ROMFS) {
            entry = romfs_get_fentry(ctx->romfs_ctx.files, file_offset);
        } else {
            entry = romfs_get_fentry(ctx->bktr_ctx.files, file_offset);
        }

        filepath_copy(cur_path, dir_path);
        if (entry->name_size) {
            filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
        }

        int found_file = 1;

        /* If we're extracting... */
        uint64_t phys_offset;
        if (ctx->type == ROMFS) {
            phys_offset = ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.data_offset + entry->offset;
        } else {
            phys_offset = ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.data_offset + entry->offset;
        }
        if ((ctx->tool_ctx->action & ACTION_ONLYUPDATEDROMFS) == 0 || nca_is_romfs_file_updated(ctx, phys_offset, entry->size)) {
            if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
                printf("Saving %s...\n", cur_path->char_path);
                nca_save_section_file(ctx, phys_offset, entry->size, cur_path, Env);
            } else {
                printf("rom:%s\n", cur_path->char_path);
            }
        } else {
            found_file = 0;
        }

        found_any_file |= found_file;

        file_offset = entry->sibling;
    } while (file_offset != ROMFS_ENTRY_EMPTY);

    free(cur_path);

    return found_any_file;
}

static int nca_visit_nca0_romfs_file(nca_section_ctx_t *ctx, uint32_t file_offset, filepath_t *dir_path, Napi::Env Env) {
    filepath_t *cur_path = static_cast<filepath_t *>(calloc(1, sizeof(filepath_t)));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    int found_any_file = 0;

    do {
        romfs_fentry_t *entry = romfs_get_fentry(ctx->nca0_romfs_ctx.files, file_offset);

        filepath_copy(cur_path, dir_path);
        if (entry->name_size) {
            filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
        }

        int found_file = 1;

        /* If we're extracting... */
        uint64_t phys_offset = ctx->nca0_romfs_ctx.romfs_offset + ctx->nca0_romfs_ctx.header.data_offset + entry->offset;

        if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
            printf("Saving %s...\n", cur_path->char_path);
            nca_save_section_file(ctx, phys_offset, entry->size, cur_path, Env);
        } else {
            printf("rom:%s\n", cur_path->char_path);
        }

        found_any_file |= found_file;

        file_offset = entry->sibling;
    } while (file_offset != ROMFS_ENTRY_EMPTY);

    free(cur_path);

    return found_any_file;
}

static int nca_visit_romfs_dir(nca_section_ctx_t *ctx, uint32_t dir_offset, filepath_t *parent_path, Napi::Env Env) {
    romfs_direntry_t *entry;
    if (ctx->type == ROMFS) {
        entry = romfs_get_direntry(ctx->romfs_ctx.directories, dir_offset);
    } else {
        entry = romfs_get_direntry(ctx->bktr_ctx.directories, dir_offset);
    }
    filepath_t *cur_path = static_cast<filepath_t *>(calloc(1, sizeof(filepath_t)));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, parent_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
    }

    /* If we're actually extracting the romfs, make directory. */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        os_makedir(cur_path->os_path);
    }

    int any_files = 0;

    if (entry->file != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_romfs_file(ctx, entry->file, cur_path, Env);
    }
    if (entry->child != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_romfs_dir(ctx, entry->child, cur_path, Env);
    }

    if (any_files == 0 && ctx->type == BKTR && (ctx->tool_ctx->action & ACTION_ONLYUPDATEDROMFS)) {
        os_rmdir(cur_path->os_path);
    }


    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_dir(ctx, entry->sibling, parent_path, Env);
    }

    free(cur_path);
    return any_files;
}

static int nca_visit_nca0_romfs_dir(nca_section_ctx_t *ctx, uint32_t dir_offset, filepath_t *parent_path, Napi::Env Env) {
    romfs_direntry_t *entry = romfs_get_direntry(ctx->nca0_romfs_ctx.directories, dir_offset);
    filepath_t *cur_path = static_cast<filepath_t *>(calloc(1, sizeof(filepath_t)));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, parent_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
    }

    /* If we're actually extracting the romfs, make directory. */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        os_makedir(cur_path->os_path);
    }

    int any_files = 0;

    if (entry->file != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_file(ctx, entry->file, cur_path, Env);
    }
    if (entry->child != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_dir(ctx, entry->child, cur_path, Env);
    }

    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        any_files |= nca_visit_nca0_romfs_dir(ctx, entry->sibling, parent_path, Env);
    }

    free(cur_path);
    return any_files;
}

void nca_save_ivfc_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath, Env);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath, Env);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %" PRId32 " is corrupted!\n", ctx->section_num);
}

void nca_save_nca0_romfs_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->nca0_romfs_ctx.header.header_size == NCA0_ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_nca0_romfs_dir(ctx, 0, &fakepath, Env);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_nca0_romfs_dir(ctx, 0, dirpath, Env);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %" PRId32 " is corrupted!\n", ctx->section_num);
}

void nca_save_bktr_section(nca_section_ctx_t *ctx, Napi::Env Env) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath, Env);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    filepath_t appended_path;
                    filepath_init(&appended_path);
                    filepath_copy(&appended_path, dirpath);
                    if (ctx->tool_ctx->settings.append_section_types) {
                        filepath_set_format(&appended_path, "%s_%s", dirpath->char_path, nca_get_section_type_name(ctx->type));
                        if (appended_path.valid == VALIDITY_VALID) {
                            dirpath = &appended_path;
                        } else {
                            printf("[WARN] Failed to append section type to path\n");
                        }
                    }

                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath, Env);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %" PRId32 " is corrupted!\n", ctx->section_num);
}
