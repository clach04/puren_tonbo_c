CC = gcc

CHI_C = chi_c/src

CFLAGS = -Wno-attributes -std=c99 -I$(CHI_C)/ -I$(CHI_C)/openssh_shim -I$(CHI_C)/openssh-portable -I$(CHI_C)/openssh-portable/openbsd-compat
# If feeling spicy...
#CFLAGS += -Wall -Wextra
LDFLAGS = -lgdi32 -lcomctl32 -lcomdlg32 -lshell32


SRCS = main.c config.c encoding.c ini.c fts_fuzzy_match.c $(CHI_C)/bf01_file.c $(CHI_C)/openssh-portable/openbsd-compat/blowfish.c $(CHI_C)/openssh-portable/openbsd-compat/md5.c

TARGET = tonbo.exe

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	del $(TARGET) 2>nul
