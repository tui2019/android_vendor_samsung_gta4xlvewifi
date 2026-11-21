/*
 * mdt_stitch.c - Stitch Qualcomm split ELF (.mdt + .bXX) firmware into monolithic .mbn
 * Supports both ELF32 and ELF64 firmware images (adsp, cdsp, modem, venus, a615_zap, ipa_fws).
 */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QCOM_MDT_TYPE_MASK (7u << 24)
#define QCOM_MDT_TYPE_HASH (2u << 24)

static int copy_fd(int in_fd, int out_fd, off_t out_off, size_t max_len) {
    if (lseek(out_fd, out_off, SEEK_SET) < 0) return -1;
    uint8_t buf[65536];
    size_t done = 0;
    while (done < max_len) {
        size_t chunk = max_len - done;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        ssize_t n = read(in_fd, buf, chunk);
        if (n <= 0) break;
        ssize_t w = write(out_fd, buf, (size_t)n);
        if (w != n) return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.mdt> <output.mbn>\n", argv[0]);
        return 1;
    }
    const char *mdt_path = argv[1];
    const char *out_path = argv[2];

    int mdt_fd = open(mdt_path, O_RDONLY);
    if (mdt_fd < 0) {
        fprintf(stderr, "mdt_stitch: open %s: %s\n", mdt_path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(mdt_fd, &st) < 0 || st.st_size < 64) {
        close(mdt_fd);
        return 1;
    }

    uint8_t *mdt = malloc((size_t)st.st_size);
    if (!mdt || read(mdt_fd, mdt, (size_t)st.st_size) != st.st_size) {
        free(mdt);
        close(mdt_fd);
        return 1;
    }
    close(mdt_fd);

    if (memcmp(mdt, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "mdt_stitch: %s is not an ELF file\n", mdt_path);
        free(mdt);
        return 1;
    }

    char base[512];
    strncpy(base, mdt_path, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".mdt") == 0)
        base[blen - 4] = '\0';

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        fprintf(stderr, "mdt_stitch: create %s: %s\n", tmp_path, strerror(errno));
        free(mdt);
        return 1;
    }

    if (write(out_fd, mdt, (size_t)st.st_size) != st.st_size) {
        close(out_fd);
        unlink(tmp_path);
        free(mdt);
        return 1;
    }

    int is64 = (mdt[EI_CLASS] == ELFCLASS64);
    uint16_t phnum = 0, phentsize = 0;
    uint64_t phoff = 0;
    if (is64) {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)mdt;
        phnum = eh->e_phnum;
        phentsize = eh->e_phentsize;
        phoff = eh->e_phoff;
    } else {
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)mdt;
        phnum = eh->e_phnum;
        phentsize = eh->e_phentsize;
        phoff = eh->e_phoff;
    }

    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t p_off = 0, p_filesz = 0;
        uint32_t p_flags = 0;
        size_t off = (size_t)(phoff + (uint64_t)i * phentsize);
        if (off + phentsize > (size_t)st.st_size) break;
        if (is64) {
            const Elf64_Phdr *ph = (const Elf64_Phdr *)(mdt + off);
            p_off = ph->p_offset;
            p_filesz = ph->p_filesz;
            p_flags = ph->p_flags;
        } else {
            const Elf32_Phdr *ph = (const Elf32_Phdr *)(mdt + off);
            p_off = ph->p_offset;
            p_filesz = ph->p_filesz;
            p_flags = ph->p_flags;
        }
        if (p_filesz == 0) continue;

        char seg_path[528];
        snprintf(seg_path, sizeof(seg_path), "%s.b%02u", base, (unsigned)i);
        int sfd = open(seg_path, O_RDONLY);
        if (sfd < 0) {
            /* Fallback: if hash segment .bXX is missing, copy from packed .mdt tail */
            if ((p_flags & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH && p_off > 0) {
                uint64_t ehdr_sz = 0;
                if (is64) {
                    const Elf64_Phdr *ph0 = (const Elf64_Phdr *)(mdt + phoff);
                    ehdr_sz = ph0->p_filesz;
                } else {
                    const Elf32_Phdr *ph0 = (const Elf32_Phdr *)(mdt + phoff);
                    ehdr_sz = ph0->p_filesz;
                }
                if (ehdr_sz + p_filesz <= (uint64_t)st.st_size) {
                    if (lseek(out_fd, (off_t)p_off, SEEK_SET) < 0 ||
                        write(out_fd, mdt + ehdr_sz, (size_t)p_filesz) != (ssize_t)p_filesz) {
                        close(out_fd);
                        unlink(tmp_path);
                        free(mdt);
                        return 1;
                    }
                }
            }
            continue;
        }
        if (copy_fd(sfd, out_fd, (off_t)p_off, (size_t)p_filesz) < 0) {
            fprintf(stderr, "mdt_stitch: failed copying %s\n", seg_path);
            close(sfd);
            close(out_fd);
            unlink(tmp_path);
            free(mdt);
            return 1;
        }
        close(sfd);
    }

    fsync(out_fd);
    close(out_fd);
    free(mdt);
    if (rename(tmp_path, out_path) < 0) {
        unlink(tmp_path);
        return 1;
    }
    return 0;
}
