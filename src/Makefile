CC = gcc

CHI_C = chi_c/src

CFLAGS = -Wno-attributes -std=c99 -I$(CHI_C)/ -I$(CHI_C)/openssh_shim -I$(CHI_C)/openssh-portable -I$(CHI_C)/openssh-portable/openbsd-compat
# If feeling spicy...
#CFLAGS += -Wall -Wextra
LDFLAGS = -lgdi32 -lcomctl32 -lcomdlg32 -lshell32


SRCS = main.c config.c encoding.c ini.c fts_fuzzy_match.c $(CHI_C)/bf01_file.c $(CHI_C)/openssh-portable/openbsd-compat/blowfish.c $(CHI_C)/openssh-portable/openbsd-compat/md5.c

RES_OBJS = tonbo_res.o

TARGET = tonbo.exe

$(TARGET): $(SRCS) $(RES_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tonbo_res.o: tonbo.rc puren_tonbo_red_icon_32x32.ico
	windres -O coff -o $@ tonbo.rc

clean:
	del $(TARGET) tonbo_res.o 2>nul
