# rmtc_bot —— RMTC 图灵对决赛参赛程序骨架（C++ / ROS 2 Humble）

**⚠️ 这份代码在本机（Windows）没有编译器，未经过编译验证。** 第一次 `colcon build` 大概率需要改 1～2 处小问题（多半是 include 或类型推导），属于正常现象，不要慌。

---

## 1. 它做了什么

| 模块 | 文件 | 说明 |
|---|---|---|
| 感知解析 | `src/bot.hpp` | 解析 `/team_X/perception`，把每个实体自带的 9×9 相对字符串还原成**绝对坐标** |
| 地图记忆 | `src/bot.hpp` | 障碍永久记住；资源看到就记、在视野内消失即视为被采走；敌人不假设实时真值 |
| 决策 | `src/bot.hpp` | 攻击判定 / 残血逃离 / 任务点 & 资源 BFS / 未知区域探索 / 幻术方向反转 |
| AI 网关 | `src/llm.{hpp,cpp}` | libcurl 异步请求，1.5s 超时，成功才计配额，失败静默降级 |
| 主程序 | `src/main.cpp` | 话题收发、tick/session_id 原样回填、换场清缓存、注册心跳 |

**已实现**：双向通信、tick 回填、9×9 坐标还原、BFS 寻路、攻击/防御/等待、任务点、AI 事件理解、危险区规避、幻术反转。
**没实现**（留给你按策略补）：更聪明的对抗（预判、卡位）、多车协同（你们是单车）、更复杂的事件收益计算（天使恩泽回血点、无双堡垒驻守）。

---

## 2. 装依赖

```bash
sudo apt update
sudo apt install -y \
  nlohmann-json3-dev libcurl4-openssl-dev \
  ros-humble-rclpy ros-humble-std-msgs ros-humble-rmw-fastrtps-cpp
```

> `nlohmann-json3-dev` 和 `libcurl4-openssl-dev` 必须在**编译前**装好，否则 `find_package` 直接失败。

## 3. 编译

```bash
# 放到工作空间的 src 下
cp -r rmtc-bot ~/ros2_ws/src/
cd ~/ros2_ws
colcon build --symlink-install --packages-select rmtc_bot
source install/setup.bash
ros2 pkg executables rmtc_bot      # 应输出 rmtc_bot contestant
```

只改 `config/decision_config.json` 不需要重编（它是运行时读的）。

---

## 4. 虚拟机网络（最关键的一步）

裁判机和你的虚拟机必须**在同一网段裸通**，DDS 才能互相发现：

| 虚拟化 | 设置 |
|---|---|
| VMware | 虚拟机设置 → 网络适配器 → **桥接（Bridged）**，勾选"复制物理网络连接状态" |
| VirtualBox | 设置 → 网络 → 连接方式 **Bridged Adapter**，界面名称选你实际连网的那块网卡 |

**不要用 NAT**——NAT 下裁判机主动连不进你的虚拟机，表现就是"有话题、没感知"。

自检：

```bash
ip a                      # 应该是 192.168.x.x 这种内网地址，不是 10.0.2.15（NAT 典型地址）
ping <裁判机IP>            # 通
```

防火墙如果开着，先关掉或放行 DDS：
```bash
sudo ufw status && sudo ufw disable     # 比赛环境内网，直接关最省事
```

---

## 5. 联调步骤（按裁判教程 v1.1）

```bash
# 1) 准备测试裁判
sha256sum -c rmtc_practice.sha256
tar xzf rmtc_practice.tar.gz && cd rmtc_practice
./start_referee.sh --stage 1
# 浏览器打开 http://127.0.0.1:8080

# 2) 先用参考车确认环境（不代表你的程序通过）
./start_bot.sh A '环境验证车'
# 网页点「开始 / 继续」，看到 Tick 推进即 OK，然后 Ctrl+C 停掉参考车

# 3) 换成自己的程序（同一席位不能同时跑两个！）
export ROS_DOMAIN_ID=27 ROS_LOCALHOST_ONLY=0 RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export RMTC_SEAT=A RMTC_TEAM_TOKEN=practice-A
export RMTC_AI_URL=http://127.0.0.1:8080/v1/chat/completions
ros2 run rmtc_bot contestant
```

**通过标准**（教程第 3.4 节）：默认节奏下持续收感知 → 发动作 → 收到 `last_action` 回执；能自主移动采集；重开后用新 `session_id` 继续；暂停重启后能重新定位。

第二阶段：
```bash
./start_referee.sh --stage 2 --bots B,C,D     # C,D 也可以不加
```
真实 LLM 联调：先退出裁判，`export PRACTICE_LLM_URL/PRACTICE_LLM_KEY/PRACTICE_MODEL`，再 `./start_referee.sh --stage 2 --ai-mode real`。

---

## 6. 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `RMTC_SEAT` | `A` | 本场席位，决定 `/team_A|B|C|D/...` |
| `RMTC_TEAM_TOKEN` | 空 | **必填**，本场席位令牌（AI 鉴权 + 真实调用资格） |
| `RMTC_TEAM_NAME` | `队伍名称` | 注册话题里的显示名，1–96 字节 |
| `ROS_DOMAIN_ID` | `27` | 正式比赛用主办方公布值 |
| `RMTC_AI_URL` | `http://127.0.0.1:8000/v1/chat/completions` | 联调时改成 `127.0.0.1:8080`，比赛用公布地址 |
| `RMTC_AI_MODEL` | 空 | 留空 = 用主办方默认模型（推荐留空） |
| `RMTC_AI_MAX_CALLS` | `40` | 自己设的配额上限，低于官方上限（示例 100） |
| `RMTC_DECISION_CONFIG` | `decision_config.json` | 策略文件路径 |

---

## 7. 现场唯一能改的东西

`config/decision_config.json` 里的 `decision_type`：

| 取值 | 行为 |
|---|---|
| `balanced` | 默认：血量 ≥40 时能打就打，否则跑 |
| `collect` | 只采集，绝不主动开战（分数落后时保底用） |
| `defensive` | 血量 ≥70 才反击 |
| `aggressive` | 血量 ≥15 就打 |

**这 4 个枚举必须赛前向裁判登记**，现场只能改这个值，不能改代码/算法/Prompt。

---

## 8. 排查对照表

| 现象 | 原因 |
|---|---|
| 网页有"感知 Tick"，程序收不到 | 虚拟机 NAT / ROS_DOMAIN_ID 不对 / 席位写错 |
| 有"动作 Tick"但"执行 Tick"不涨 | tick 或 session_id 没和感知一致（看 `self.last_action.error`） |
| `session_mismatch` | 缓存了上一场的 session_id，或自己生成了 session_id |
| 车不动 | 检查 `last_action.moved=false` 与 `error`；`E_BLOCKED` 说明前方被占 |
| AI 一直 401 | 令牌席位不匹配，或 Bearer 头没带 |
| AI 429 | 配额用尽或并发超限（本程序限制 1 个在途） |
| 编译报错找不到 nlohmann/json.hpp | 忘了 `apt install nlohmann-json3-dev` |
| 编译报错找不到 curl | 忘了 `libcurl4-openssl-dev` |

日志位置：网页"本场日志"，或 `~/.rmtc-practice/` 下的 `communication.jsonl`、`ai.jsonl`。

---

## 9. 交付前检查

- [ ] 两个阶段都在默认 Tick 节奏下跑通
- [ ] 感知 → 动作 → 回执 三个环节都验证过（截图/录屏，10 月 31 日 20:00 前交）
- [ ] 第二阶段用过**真实** LLM（不是模拟模式）
- [ ] 暂停恢复、重开、进程重启后都能恢复控制
- [ ] 代码里没有任何写死的令牌/场次 ID/地图/事件时间
- [ ] 材料里不含密钥或正式令牌
