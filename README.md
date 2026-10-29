# rmtc_bot

RMTC 机甲大师图灵对决赛参赛程序：**ROS 2 Humble / C++17**。

一个节点完成整套决策：订阅 `/team_X/perception`（JSON 感知）→ 本地计算 → 向
`/team_X/action` 回填同一 `tick` 与 `session_id`。AI 只用于理解自然语言公告，动作
全部本地计算，因此 AI 超时不会阻塞动作窗口。

当前版本标记：`v9-20261028-unstick`

---

## 1. 目录结构

```
rmtc-bot/
├── CMakeLists.txt              # 编译与安装规则
├── package.xml                 # 包声明与依赖
├── README.md
├── config/
│   └── decision_config.json    # 现场唯一允许修改的文件（decision_type）
├── launch/
│   └── contest.launch.py       # 等价于 ros2 run，方便带 output=screen
├── scripts/
│   └── run.sh                  # 设好环境变量后一键启动
└── src/
    ├── bot.hpp                 # 感知结构体 + 解析 + 地图记忆 + BFS + 决策
    ├── llm.hpp / llm.cpp       # 统一 AI 网关客户端（libcurl、异步、超时降级）
    └── main.cpp                # 节点：话题收发、回填 tick、回执学习、异常兜底
```

| 文件 | 职责 |
| --- | --- |
| `src/bot.hpp` | `Perception` 解析（含 9×9 相对坐标还原）、`World` 地图记忆与两段式 BFS、五级优先级决策 |
| `src/llm.cpp` | POST `<GATEWAY>/v1/chat/completions`，非流式，1.5 s 超时，成功才计配额 |
| `src/main.cpp` | QoS、注册心跳、换场重置、从 `last_action` 学习障碍、整包 try/catch |

---

## 2. 依赖

```bash
sudo apt update
sudo apt install -y \
  nlohmann-json3-dev libcurl4-openssl-dev \
  ros-humble-rclcpp ros-humble-std-msgs ros-humble-rmw-fastrtps-cpp \
  python3-colcon-common-extensions
```

| 依赖 | 用途 | 缺失时的现象 |
| --- | --- | --- |
| `nlohmann-json3-dev` | 解析感知/构造动作 JSON | `fatal error: nlohmann/json.hpp: No such file` |
| `libcurl4-openssl-dev` | 调用 AI 网关 | `undefined reference to curl_easy_*` |
| `ros-humble-rclcpp` | 节点与话题 | `find_package(rclcpp)` 失败 |

---

## 3. 编译

```bash
# 放进工作空间的 src 下（目录名必须是 rmtc-bot 或任意，包名以 package.xml 为准）
cp -r rmtc-bot ~/ros2_ws/src/

source /opt/ros/humble/setup.bash
cd ~/ros2_ws
colcon build --packages-select rmtc_bot
source install/setup.bash

ros2 pkg executables rmtc_bot      # 应输出：rmtc_bot contestant
```

> 改了 `src/` 下的代码必须重新 `colcon build` 并 `source install/setup.bash`。
> 只改 `config/decision_config.json` 不需要重编（运行时读取）。

---

## 4. 运行

### 4.1 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `RMTC_SEAT` | `A` | 本场席位，决定 `/team_A|B|C|D/...` |
| `RMTC_TEAM_TOKEN` | 空 | **必填**，本场席位令牌（AI 鉴权）；换场会重新签发 |
| `RMTC_TEAM_NAME` | `队伍名称` | 注册话题的显示名，1–96 字节 |
| `ROS_DOMAIN_ID` | 27 | 与裁判一致 |
| `ROS_LOCALHOST_ONLY` | 0 | 跨机通信必须为 0 |
| `RMW_IMPLEMENTATION` | 系统默认 | 建议 `rmw_fastrtps_cpp`，与裁判一致 |
| `RMTC_AI_URL` | `http://127.0.0.1:8000/v1/chat/completions` | 联调时改 `127.0.0.1:8080`，比赛用主办方地址 |
| `RMTC_AI_MODEL` | 空 | 留空 = 主办方默认模型（推荐留空） |
| `RMTC_AI_MAX_CALLS` | 40 | 本地配额上限（正式配额以赛前公告为准） |
| `RMTC_DECISION_CONFIG` | `decision_config.json` | 策略文件路径 |
| `RMTC_DEBUG` | 空 | 设为任意值 → 每 Tick 打一行日志 |

### 4.2 启动

```bash
export ROS_DOMAIN_ID=27 ROS_LOCALHOST_ONLY=0 RMW_IMPLEMENTATION=rmw_fastrtps_cpp \
       RMTC_SEAT=A RMTC_TEAM_TOKEN=practice-A \
       RMTC_AI_URL=http://127.0.0.1:8080/v1/chat/completions

source /opt/ros/humble/setup.bash && source ~/ros2_ws/install/setup.bash
ros2 run rmtc_bot contestant
```

或用脚本（会做同样的环境设置）：

```bash
chmod +x scripts/run.sh
RMTC_TEAM_TOKEN=practice-A ./scripts/run.sh
```

启动第一行会打印版本标记，**这是确认跑的是不是最新二进制的唯一依据**：

```
======== BUILD v9-20261028-unstick ========
seat=A mode=balanced ai=http://127.0.0.1:8080/v1/chat/completions token=(已设置)
```

---

## 5. 接口（按《选手通信协议》）

### 5.1 话题与 QoS

| 话题 | 方向 | 可靠性 | 持久性 | 深度 |
| --- | --- | --- | --- | --- |
| `/team_X/perception` | 订阅 | RELIABLE | VOLATILE | 1 |
| `/team_X/action` | 发布 | RELIABLE | VOLATILE | 1 |
| `/team_X/registration` | 发布 | RELIABLE | TRANSIENT_LOCAL | 1 |
| `/referee/mission` | 订阅 | RELIABLE | TRANSIENT_LOCAL | 1 |
| `/referee/event_text` | 订阅 | RELIABLE | TRANSIENT_LOCAL | **128** |
| `/referee/team_registry` | 订阅 | RELIABLE | TRANSIENT_LOCAL | 1 |

所有消息类型均为 `std_msgs/msg/String`；感知/动作/注册的 `data` 是 JSON，任务与事件公告是纯文本。

### 5.2 动作

```json
{"tick":148,"session_id":"example-match","action":"move","direction":"right"}
{"tick":148,"session_id":"example-match","action":"attack","direction":"up"}
{"tick":148,"session_id":"example-match","action":"defend"}
{"tick":148,"session_id":"example-match","action":"wait"}
```

- `tick` 必须是**整数**且等于所依据感知的 `tick`；`session_id` 必须是**字符串**，原样回填
- `move`/`attack` 必须带方向；单条消息 ≤ 4096 字节
- 每 Tick 只发一条，不做批量补发

### 5.3 感知里最容易搞错的两处

1. `self.position` 是**绝对坐标数组** `[x,y]`；`visible_entities[].position` 是**九行字符串**
   （81 格 + 8 换行 = 89 字符，无末尾换行，`1` 固定在第 5 行第 5 列，`2` 是该实体）。
2. 阵亡时 `visible_entities` 为空，**不能**据此认为"视野内的资源都被采走了"。

---

## 6. 决策逻辑

### 6.1 五级优先级

| 优先级 | 行为 | 条件 | 日志 `goal=` |
| --- | --- | --- | --- |
| 1 | 攻击 | 直线 1–2 格内有敌车，且（己方 HP ≥ 40 或敌车 ≤ 20） | `attack` |
| 2 | 脱离 | 受击后 HP ≤ 30 | `flee` |
| 3 | 去任务点 | 资源价值 ≥ 120，或 tick ≥ 400 | `reach` |
| 4 | 采集 | 按"距离 + 敌人威胁"排序，逐个试可达性 | `res` |
| 5 | 猎杀 / 探索 | 无资源可采：先找能打赢的敌车，再探索未知区 | `hunt` / `explore` |

优先级 3 的依据是计分公式：资源分满分需 120 价值（30 分），而到达任务点只有 5 分。

### 6.2 两个工程细节

- **两段式 BFS**：先只走"已知可通行"格，走不通再允许穿过未知区，减少撞墙浪费的 Tick。
- **从回执学障碍**：`self.last_action` 报 `moved=false` + `E_BLOCKED`/`E_OOB` 时，把那个格子标记为
  临时禁行 15 Tick，日志打印 `学到障碍：(x,y) 走不通，回避 15 Tick`。

### 6.3 可调参数

`config/decision_config.json` 只放一个字段（现场只允许改它）：

```json
{ "decision_type": "balanced" }
```

| 取值 | 行为 |
| --- | --- |
| `balanced` | 默认：HP ≥ 40 能打就打，否则脱离 |
| `collect` | 只采集，绝不主动开战 |
| `aggressive` | HP ≥ 15 就打 |
| `defensive` | HP ≥ 70 才反击 |

> 这四个枚举必须在**赛前**向裁判登记；现场只能改这个值，不得改代码/算法/Prompt。

代码内还可调（需重编）：`kMaxCallsPerSession`（本场 AI 调用上限，默认 25）、
`kAskCooldownTicks`（两次调用最小间隔，默认 40 Tick）。

---

## 7. 本地验证

### 7.1 用官方练习裁判

```bash
# 终端 1：裁判（第二阶段 + 三台陪练）
cd ~/rmtc/dist/rmtc_practice && ./start_referee.sh --stage 2 --bots B,C,D
# 浏览器打开 http://127.0.0.1:8080

# 终端 2：本程序（环境变量见 4.2）
ros2 run rmtc_bot contestant

# 浏览器：部署新练习 → 开始 / 继续
```

### 7.2 三个灯都要亮

| 位置 | 期望 | 不亮说明 |
| --- | --- | --- |
| 终端日志 | `tick=0 hp=100 pos=(1,1) act=move right goal=res` 持续刷新 | 没收到感知（域名/席位/网络） |
| 网页"感知 Tick" | 涨 | 裁判没在跑 |
| 网页"动作 Tick" | 涨 | 动作没发出去或发错话题 |
| 网页"**执行 Tick**" | 涨 | `tick`/`session_id` 没对上，看 `self.last_action.error` |

### 7.3 已知实测结果

| 场景 | 结果 |
| --- | --- |
| 第一阶段（18×18，A 单车） | tick=90 采满 200/200 价值 → **40/40** |
| 第二阶段（A + 三台陪练） | 资源 60 + 有效伤害 900 + 三项任务 → **约 45/60** |

---

## 8. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 网页有感知、程序无日志 | 域号不同 / `ROS_LOCALHOST_ONLY=1` / 跨机网络不通 | 两边统一 `ROS_DOMAIN_ID`，设 `ROS_LOCALHOST_ONLY=0`，关掉多余网卡 |
| 有动作 Tick、执行 Tick 不涨 | `tick` 或 `session_id` 与感知不一致 | 不要自己生成 session_id，逐帧回填 |
| 程序一直 `act=wait` | 没收到感知，或 `decide` 找不到路径 | 看日志有没有 `goal=none`；确认感知在刷新 |
| 换场后车不动 | 新场次 tick 从 0 开始 | 已处理：换场重置 `last_tick_`，且 `tick==0` 无条件接受 |
| AI 一直 401 | 令牌与席位不匹配 | 用本场席位令牌；换场后令牌会失效 |
| AI 429 | 配额耗尽或并发超限 | 程序限 1 个在途；降低调用频次或换场 |
| 编译报 `nlohmann/json.hpp` 找不到 | 少装 `nlohmann-json3-dev` | 见第 2 节 |
| 改了代码但行为没变 | 没重编 / 跑的是旧二进制 | 看启动第一行 `BUILD` 标记；`colcon build --packages-select rmtc_bot` |

---

## 9. 已知限制

- **事件收益未充分利用**：目前只把随机事件当作"危险区"绕开，"天使恩泽"回血点与"天下无双"堡垒
  的收益没有专门跑一趟。
- **资源采集依赖对手速度**：18×18 图上 4 台车抢 16 个资源，开局约 10 秒即被抢空，之后靠猎杀补分。
- **AI 只验证了链路**：本地用模拟网关验证了 HTTP 与解析，真实模型效果需在主办方网关上再调。
- **对抗靠"能打就打"**：没有用双方 HP/距离做换血评估，劣势时是"掉到 30 HP 才跑"。

---

## 10. 版本记录

| 标记 | 主要改动 |
| --- | --- |
| `v5` | 基线：接入 + 解析 + 采集 |
| `v6` | 采集优先：资源价值 ≥120 才去任务点 |
| `v7` | 资源抢光后主动猎杀（`goal=hunt`） |
| `v8` | 异常兜底、阵亡不清记忆、两段式 BFS |
| `v9` | 从 `last_action` 学障碍，修复长时间撞墙 |

每次改代码都要同步修改 `src/main.cpp` 里的 `kBuildTag`，否则无法确认现场跑的是哪一版。
