/* 006_equiv.c —— `simplexc/corpus/src/006.si`（2 输入 NAND，3 个组件）的 C 等价物。
 *
 * 用途（M1.5 收益探针）
 * --------------------
 * 让 clang -O2 生成一份「语义等价物」，再拿 M2 差分宿主跟参考产物逐 tick 对账，
 * 顺便量两边在同一驱动下的吞吐。地址表、枚举序号与 006.si 逐字一致 —— 宿主
 * `simplexc/tools/host/simspec.py` 解析的是 006.si，所以这里不能自己另立一套。
 *
 * 写法约束（为什么这么绕）
 * ----------------------
 * · **不用 CRT**：这个目标文件会被 `tcblob` 链进 blob，`sprintf`/`memcpy`/`windows.h`
 *   都是 `ExternResolver` 解析不了的未定义符号。字符串长度、内存清零全部自己写循环。
 * · **游戏内存一律 `volatile`**：命令块由游戏（宿主）在另一个线程里改，
 *   编译器不许把 `get_command(ctl_command_id)` 提出来一次算完 —— 那会让轮询永远看不到新命令。
 * · 只有两个 kernel32 依赖（`Sleep` / `GetSystemTimePreciseAsFileTime`），参考产物走的是
 *   同一组函数的 DLL thunk —— 节奏类是宿主显式白名单，不进状态比较（见 docs/facts.md）。
 *
 * 编译（clang 必须来自 simplexc/.deps/llvm/bin，不许 g++ 顶替）
 * --------------------------------------------------------------
 *   simplexc/.deps/llvm/bin/clang.exe --target=x86_64-pc-windows-gnu -O2 -ffreestanding \
 *       -fno-stack-protector -c -o 006_equiv.o 006_equiv.c
 *   simplexc/build/tcblob.exe --obj 006_equiv.o --obj simplexc/build/objs/entry_shim.o \
 *       --obj simplexc/build/objs/rt_min.o --entry __tc_entry --out 006_ours.bin
 *
 * `-DTC_006_NO_FOLD`：把电路的状态变量标成 `volatile`，禁止 clang 常量折叠
 * （探针用它量「同样的每周期工作量、不同代码生成质量」那一档，见 tools/probe_006.py）。
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed long long i64;

/* ---- 游戏内存地址（与 006.si 里的 `Ptr`/`raw_cast` 一致）---- */
#define A_COMMANDS       0x232A6FF0000ULL
#define A_SETTINGS       0x232A7000000ULL
#define A_INPUT_REPLAY   0x232A7010000ULL
#define A_ERROR_BUFFER   0x232AA920000ULL
#define A_CTL_RESET      0x232AABC0000ULL
#define A_SIM_STATE      0x232A9F50000ULL
#define A_KB_CHAR        0x232AAB30030ULL
#define A_KB_COORD       0x232AAB33030ULL

/* ---- 枚举序号（按 006.si 的声明顺序从 0 起，与宿主 simspec 的取值一致）---- */
enum {
    CMD_RUN = 0, CMD_REFRESH = 1, CMD_MODE_RESET = 2, CMD_QUIT_SIMULATION = 3
};
enum {
    CTL_COMMAND = 0, CTL_COMMAND_ID = 1, CTL_CYCLE_SPEED_MS = 2, CTL_EXIT = 3,
    CTL_LEVEL_MANUAL_INPUT = 4, CTL_LEVEL_MANUAL_INPUT_ID = 5, CTL_TEST = 6
};
enum {
    ST_SIM_CYCLE = 0, ST_SIM_TARGET_CYCLE = 1, ST_SIM_TEST_RESULT = 2,
    ST_SIM_LAST_COMMAND_ID = 3, ST_SIM_ERROR_COMPONENT = 4,
    ST_SC_COMPONENT_ID_1 = 5, ST_SC_COMPONENT_ID_2 = 6,
    ST_SIM_RUNNING = 14
};
enum { TR_PASS = 0, TR_WIN = 1, TR_FAIL = 2 };

#define CYCLE_PAST_FAIL 0ULL
#define SPEED_CAP       10000000000000ULL          /* 10 ghz，防溢出 */
#define FILE_TIME_UNIX  116444736000000000ULL      /* 1970-01-01 的 FILETIME（100ns） */

static volatile u64 *const commands = (volatile u64 *)A_COMMANDS;
static volatile u64 *const settings = (volatile u64 *)A_SETTINGS;
static volatile u64 *const input_replay = (volatile u64 *)A_INPUT_REPLAY;
static volatile u64 *const ctl_input_replay_reset = (volatile u64 *)A_CTL_RESET;
static volatile u8 *const error_buffer = (volatile u8 *)A_ERROR_BUFFER;
static volatile u8 *const sim_state = (volatile u8 *)A_SIM_STATE;
static volatile u8 *const keyboard_char = (volatile u8 *)A_KB_CHAR;
static volatile u8 *const keyboard_coord = (volatile u8 *)A_KB_COORD;

/* 程序自己的状态。参考产物把这些放在 DLL 的 .bss（0x131280 那一带）—— 宿主不比对那块，
   我们放自己的 .bss 即可：映射到的内存地址与内容都不同，但都不是「游戏可见状态」。
   `-DTC_006_NO_FOLD` 时电路的输入/输出加 volatile，逼编译器每周期真的读一次。*/
#ifdef TC_006_NO_FOLD
#define LEVEL volatile
#else
#define LEVEL
#endif
static LEVEL u32 level_input_input;      /* Input { input: U2 } */
static LEVEL u8 level_output_output;     /* Output { output: U1, output_is_z: Bool } */
static LEVEL u8 level_output_is_z;

static i64 cycle = -1;
static u64 last_command_id = 0;
static u64 last_level_manual_input = 0;
static u64 time_component_last = 0;

extern void thread_exit(void);
/* 两个 kernel32 依赖。x86_64 只有一个调用约定，不需要 `__stdcall` 之类的修饰；
   这里必须是**裸名 extern**（不能带 `dllimport`）—— `dllimport` 会变成 `__imp_Sleep`，
   而装配器给 `__imp_*` 造的是 8 字节数据槽、给裸名造 `movabs rax,imm64 ; jmp rax` 代码桩。*/
extern void GetSystemTimePreciseAsFileTime(u64 *ft);
extern void Sleep(u32 ms);

/* ---- 仿真协议的两个访问器（006.si 里就是这两行）---- */
static u64 get_command(u64 idx) { return commands[idx]; }
static u64 get_setting(u64 idx) { return settings[idx]; }
static void set_setting(u64 idx, u64 value) { settings[idx] = value; }

static void memory_clear(volatile u8 *dst, u64 n) {
    for (u64 i = 0; i < n; i++) dst[i] = 0;
}

static void set_error(const char *text) {
    u64 len = 0;
    while (text[len] != 0) len++;
    *(volatile u16 *)error_buffer = (u16)len;          /* 长度写在缓冲头 2 字节 */
    for (u64 i = 0; i < len; i++) error_buffer[i + 2] = (u8)text[i];
}

static u64 get_time(void) {
    u64 ft = 0;
    GetSystemTimePreciseAsFileTime(&ft);
    /* `Time` 的单位是**纳秒**（不是毫秒）。三个从参考产物里读出来的旁证：
       · 目标周期的变量叫 `nanos_per_cycle`；
       · `speed = min(ctl_cycle_speed_ms, 10^13)` 那句注释写「Cap at 10 ghz」→ speed 是 Hz，
         而 `get_target_cycle` 的结果 = (t/10^6)*speed/10^6 = t*10（t 以 ns 计）= 10 GHz 的周期数；
       · 实测：单位当 ms 时我们的产物会在「跑过头就睡」那个循环里**空转一百万圈**（必须
         等 elapsed 跨过 10^6 才退出），而参考产物同一 tick 只多一次睡眠 —— 只有 ns 能解释。*/ 
    return (ft - FILE_TIME_UNIX) * 100ULL;          /* FILETIME = 100ns 单位 → ×100 = ns */
}

static void sleep_ms(u64 ms) { Sleep((u32)ms); }   /* `1.ms()` = 10^6 ns → Sleep(1) */

static void reset_sim(void) {
    time_component_last = 0;
    set_error("");
    /* `global_seed = Seed(get_command(ctl_test) + 1)` 落在 DLL 的 .bss，006 不用随机数，
       且宿主不比对那块 → 不复刻（见 notes：它是参考产物里 0x131280 那 9 处 movabs 之一）。*/
    last_level_manual_input = 0;
    set_setting(ST_SIM_CYCLE, ~0ULL);                 /* #U64_HIGH = -1 */
    set_setting(ST_SC_COMPONENT_ID_1, 0);
    set_setting(ST_SC_COMPONENT_ID_2, 0);
    set_setting(ST_SIM_TEST_RESULT, TR_PASS);
    cycle = -1;
    memory_clear(keyboard_char, 8192);
    memory_clear(keyboard_coord, 8192);
    memory_clear(sim_state, 263);
}

static void mode_refresh(void) {
    /* 引脚 1（com_level_input_2_pin）从 input_replay[0] 取两位 */
    u64 value_id256 = input_replay[0];
    sim_state[256] = (u8)(value_id256 & 1);
    sim_state[257] = (u8)(value_id256 & 1);
    u64 value_id258 = value_id256 >> 1;
    sim_state[258] = (u8)(value_id258 & 1);
    sim_state[259] = (u8)(value_id258 & 1);
    /* 组件 3（com_nand_bit）：从**内存**里读回刚写的两位（参考产物也是回读）*/
    u8 value_id260 = (u8)(~(sim_state[256] & sim_state[258]) & 1);
    sim_state[260] = value_id260;
    sim_state[261] = value_id260;
}

static void mode_run(i64 target_cycle) {
    if (get_setting(ST_SIM_TEST_RESULT) == TR_WIN) return;
    if (get_setting(ST_SIM_TEST_RESULT) == TR_FAIL
        && cycle + 1 >= (i64)CYCLE_PAST_FAIL) return;
    if (get_setting(ST_SC_COMPONENT_ID_1) != 0) return;

    u8 halt_run = 0;
    u64 original_start_time = 0;
    u64 orig_start_cycle = (u64)cycle;
    u64 last_time = original_start_time;
    u64 burst_cycles = 1;
    u64 nanos_per_cycle = 0;
    u64 speed = 1;
    u64 last_target_speed = 0;

    while (get_command(CTL_COMMAND_ID) == last_command_id && !halt_run
           && target_cycle > cycle) {
        u64 target_speed = get_command(CTL_CYCLE_SPEED_MS);
        if (target_speed < 1) target_speed = 1;
        if (target_speed != last_target_speed) {
            original_start_time = get_time();
            orig_start_cycle = (u64)cycle;
            last_time = original_start_time;
            u64 requested = get_command(CTL_CYCLE_SPEED_MS);
            speed = requested < SPEED_CAP ? requested : SPEED_CAP;
            last_target_speed = target_speed;
        }

        i64 burst_target = cycle + (i64)burst_cycles;
        i64 burst_target_cycle = target_cycle < burst_target ? target_cycle : burst_target;
        /* 一个周期的电路求值：1 个输入引脚 + 1 个 NAND + 1 个输出引脚。
           `get_input`/`check_output` 都不在 scope（006.si 的捕获里是 `#if in_scope(...)`），
           所以 level_input 恒为初值 0；`on_ui_update` 同理 → 这三处都不生成代码。

           `-DTC_006_HOT_CYCLE`：把每周期计数器先提到寄存器、循环结束再写回全局。
           语义完全一样（循环体内没有任何人读 `cycle`），但它量出来是**四倍**的差：
           全局 `cycle` 与「volatile 的游戏内存」可能同址，LLVM 就不敢把它留在寄存器里，
           于是每周期多一次 store→load 链（~5 cycle vs ~1 cycle）。值与实测见 notes。*/
#ifdef TC_006_HOT_CYCLE
        i64 hot = cycle;
        i64 *counter = &hot;                 /* 指向局部 → LLVM 会把它当寄存器变量 */
#else
        i64 *counter = &cycle;
#endif
        while (*counter < burst_target_cycle) {
            u32 input_value = level_input_input;
            u8 vid256 = (u8)(input_value & 1);
            u8 vid258 = (u8)((input_value >> 1) & 1);
            input_replay[0] = (u64)input_value;
            u8 vid260 = (u8)(~(vid256 & vid258) & 1);
            level_output_output = vid260;
            level_output_is_z = 0;
            *counter += 1;
        }
#ifdef TC_006_HOT_CYCLE
        cycle = hot;
#endif

        set_setting(ST_SIM_CYCLE, (u64)cycle);

        u64 time_now = get_time();
        u64 time_diff = time_now - last_time;
        nanos_per_cycle = time_diff / burst_cycles;
        /* `33.ms()` = 33 × 10^6 **纳秒** —— Time 的单位是 ns，`.ms()` 是换算不是后缀。
           写成 33（当纳秒用）时这条判定永远成立「爆发太久」→ `burst_cycles` 被钉在 1：
           实测目标 2000 个 cycle 要 2000 次爆发（时钟回调 63 → 2002 次，时间 12 ms → 408 ms）。*/
        if (time_now - last_time > 33 * 1000000ULL) {
            burst_cycles = burst_cycles > 1 ? burst_cycles - 1 : 1;
        } else {
            burst_cycles += 1;
        }
        last_time = time_now;

        /* 跑得比时钟快就睡一会儿 —— 宿主里永远不会进（速度封顶让目标周期远超实际周期）*/
        u64 run_target_cycle = orig_start_cycle
            + ((time_now - original_start_time) / 1000000ULL) * speed / 1000000ULL;
        while ((i64)run_target_cycle < cycle
               && get_command(CTL_COMMAND_ID) == last_command_id) {
            sleep_ms(1);
            cycle -= 1;
            mode_refresh();
            cycle += 1;
            run_target_cycle = orig_start_cycle
                + ((get_time() - original_start_time) / 1000000ULL) * speed / 1000000ULL;
        }
    }
}

static void run_sim(void) {
    for (;;) {
        reset_sim();
        /* 003 的 `get_input`/`check_output` 是空函数体、且 `in_scope(...)` 为假 → 不生成代码；
           `on_manual_input` 那一块同理，`manual_input_seen` 永远是 false → 不实现。*/
        for (;;) {
            mode_refresh();

            while (get_command(CTL_COMMAND_ID) == last_command_id) sleep_ms(1);

            last_command_id = get_command(CTL_COMMAND_ID);   /* 先读命令号，再读命令（防竞争）*/
            set_setting(ST_SIM_LAST_COMMAND_ID, last_command_id);
            u64 command = get_command(CTL_COMMAND);

            if (command == CMD_RUN) {
                i64 target_cycle = (i64)get_setting(ST_SIM_TARGET_CYCLE);
                set_setting(ST_SIM_RUNNING, 1);
                mode_run(target_cycle);
                set_setting(ST_SIM_RUNNING, 0);
            } else if (command == CMD_REFRESH) {
                /* 空 */
            } else if (command == CMD_MODE_RESET) {
                for (u64 i = 0; i < 1024; i++) input_replay[i] = ctl_input_replay_reset[i];
                break;                                       /* 回外层重跑 reset_sim */
            } else if (command == CMD_QUIT_SIMULATION) {
                return;                                      /* break run_sim：交给收尾清理 */
            }
        }
    }
}

void tc_main(void) {
    run_sim();
    thread_exit();
}
