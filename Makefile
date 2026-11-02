ifeq ($(NAME),)
$(error Should make in each lab's directory)
endif

SRCS   := $(shell find . -maxdepth 1 -name "*.c")
DEPS   := $(shell find . -maxdepth 1 -name "*.h") $(SRCS)
CFLAGS += -O1 -std=gnu11 -ggdb -Wall -Werror -Wno-unused-result -Wno-unused-value -Wno-unused-variable

.PHONY: all test clean

.DEFAULT_GOAL := all

$(NAME)-64: $(DEPS)
	gcc -m64 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(NAME)-32: $(DEPS)
	gcc -m32 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(NAME)-64.so: $(DEPS)
	gcc -fPIC -shared -m64 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(NAME)-32.so: $(DEPS)
	gcc -fPIC -shared -m32 $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f $(NAME)-64 $(NAME)-32 $(NAME)-64.so $(NAME)-32.so

include ../oslabs.mk
