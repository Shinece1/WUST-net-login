# ============================================================================
# 武汉科技大学校园网自动认证工具 —— Makefile
#
#   make            # 编译（Windows 下用 mingw32-make）
#   make clean      # 清理
#   make rebuild    # 重新编译
#
# 编译要求：C99，且必须做到 -Wall -Wextra -pedantic 零警告。
# ============================================================================

# 只在用户没有指定编译器时，覆盖 make 内置的默认值 cc
ifeq ($(origin CC),default)
  CC := gcc
endif

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
LDLIBS  ?=

# ---- 平台差异：Windows 需要链接 WinSock，可执行文件带 .exe 后缀 ----
ifeq ($(OS),Windows_NT)
  EXE       := .exe
  LDLIBS    += -lws2_32
  RM_F      := del /q /f
  # 开机自启专用：GUI 子系统（永不创建控制台窗口，零闪窗）+ 强制静默模式
  HAVE_GUI  := 1
else
  EXE       :=
  RM_F      := rm -f
endif

TARGET  := wust-auth$(EXE)
TARGETQ := wust-authw$(EXE)
SRC     := main.c
OBJ     := $(SRC:.c=.o)

.PHONY: all clean rebuild check

ifeq ($(HAVE_GUI),1)
all: $(TARGET) $(TARGETQ)
else
all: $(TARGET)
endif

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) -c -o $@ $(SRC)

# wust-authw.exe：Windows GUI 子系统版本，双击/开机启动都不会弹出黑窗口。
# 它永远以“静默 + 自动登录”方式运行，日志写在 wust-auth.log。
ifeq ($(HAVE_GUI),1)
$(TARGETQ): $(SRC)
	$(CC) $(CFLAGS) -DWUST_FORCE_QUIET -mwindows -o $@ $(SRC) $(LDLIBS)
endif

# 单独跑一遍严格告警检查（不产出文件），用于确认“无警告”要求
check:
	$(CC) -std=c99 -Wall -Wextra -pedantic -fsyntax-only $(SRC)

clean:
	-$(RM_F) $(OBJ)
	-$(RM_F) $(TARGET)
ifeq ($(HAVE_GUI),1)
	-$(RM_F) $(TARGETQ)
endif

rebuild: clean all
