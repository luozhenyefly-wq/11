# ============================================================================
#  Makefile - 空洞文件「断点续传」下载器
#  编程作业（AI 协作版） / Ubuntu 22.04.5 LTS
#
#  常用目标：
#     make            编译（默认 release，-O2）
#     make DEBUG=1    带 -g -fsanitize=address,undefined 的调试版
#     make test       跑完整测试用例（tests/test_all.sh）
#     make demo       跑一次可展示的演示（tests/demo.sh）
#     make clean      清理
# ============================================================================

CC      ?= cc
TARGET   = sparseio
SRCDIR   = src
OBJDIR   = build
TESTDIR  = tests

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes -Wvla
# 说明：不再从这里传 -D_GNU_SOURCE。每个 .c 文件开头已自行 #define _GNU_SOURCE，
# 两处同时定义会触发 gcc 的 "_GNU_SOURCE redefined" 告警（见 AI 代码审查 缺陷-1）。
CPPFLAGS += -I$(SRCDIR)

ifeq ($(DEBUG),1)
CFLAGS  := -O0 -g3
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LDFLAGS +=
LDLIBS  +=

.PHONY: all clean test demo install help dist

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD      $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "  构建完成: ./$(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	@echo "  CC      $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

## 完整测试：空洞 / 定点写入 / 断点续传 / 边界
test: $(TARGET)
	@bash $(TESTDIR)/test_all.sh

## 演示：一步一步展示空洞与续传（输出更适合截图）
demo: $(TARGET)
	@bash $(TESTDIR)/demo.sh

clean:
	@rm -rf $(OBJDIR) $(TARGET) $(TARGET).dSYM
	@rm -rf $(TESTDIR)/work
	@echo "  已清理构建产物"

install: $(TARGET)
	@install -m 0755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "  已安装到 /usr/local/bin/$(TARGET)"

## 打包源码（不带构建产物），便于提交
dist:
	@rm -f $(TARGET)-src.tar.gz
	@tar --exclude=$(OBJDIR) --exclude='$(TESTDIR)/work' \
	     --exclude=$(TARGET) --exclude=$(TARGET)-src.tar.gz \
	     -czf $(TARGET)-src.tar.gz .
	@echo "  已生成 $(TARGET)-src.tar.gz"

help:
	@echo "目标: all(默认) / test / demo / clean / install / dist"
