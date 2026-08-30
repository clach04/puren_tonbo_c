CC = gcc

CHI_C = chi_c/src

CFLAGS = -Wno-attributes -std=c99 -I$(CHI_C)/ -I$(CHI_C)/openssh_shim -I$(CHI_C)/openssh-portable -I$(CHI_C)/openssh-portable/openbsd-compat
# If feeling spicy...
#CFLAGS += -Wall -Wextra

# Debug option for right-click **Encrypt** / **Decrypt** to preserve the original file as `<filename>.<ext>.bak`. This is *not secure* (plaintext remains on disk) but aids in debugging.
#CFLAGS += -DDEBUG_PRESERVE_OLD_FILES

LDFLAGS = -lgdi32 -lcomctl32 -lcomdlg32 -lshell32


SRCS = main.c config.c encoding.c ini.c fts_fuzzy_match.c $(CHI_C)/bf01_file.c $(CHI_C)/openssh-portable/openbsd-compat/blowfish.c $(CHI_C)/openssh-portable/openbsd-compat/md5.c

RES_OBJS = tonbo_res.o

TARGET = tonbo.exe

$(TARGET): $(SRCS) $(RES_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# red_icon_all_4colors.ico - converted from https://mytoolstudio.com/tools/image/image-to-ico-converter/ 16x16, 32x32, 48x48, 64x64, 128x128, 256x256
tonbo_res.o: tonbo.rc version.h red_icon_all_4colors.ico
	windres -O coff -o $@ tonbo.rc

clean:
	del $(TARGET) tonbo_res.o 2>nul
