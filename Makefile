CC = gcc
CFLAGS = -O2 -std=gnu17 -w

# GCC 14 and later reject the int-to-pointer conversions that the provided
# main.c performs (PTR_ERR() applied to the int returned by return_pages()).
# Those diagnostics are warnings, not errors, on GCC 13 and earlier, so they
# have to be downgraded explicitly to build with both compiler generations.
# The names below exist in every GCC that has -Wint-conversion; the fallback
# tiers keep `make` working even if a toolchain rejects them.
RELAX = -Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
        -Wno-error=implicit-function-declaration -Wno-error=implicit-int

.PHONY: all clean
all:
	$(CC) $(CFLAGS) $(RELAX) -o code main.c buddy.c \
	|| $(CC) $(CFLAGS) -fpermissive -o code main.c buddy.c \
	|| $(CC) $(CFLAGS) -o code main.c buddy.c \
	|| $(CC) -O2 -w -o code main.c buddy.c

clean:
	rm -f code
