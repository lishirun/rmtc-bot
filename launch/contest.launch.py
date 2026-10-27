"""一条命令启动参赛节点（等价于 ros2 run rmtc_bot contestant）。

注意：接入信息（席位、令牌、网关地址）仍然来自环境变量，
launch 只负责把节点拉起来并把日志打在终端上。
"""
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        Node(
            package='rmtc_bot',
            executable='contestant',
            name='rmtc_contestant',
            output='screen',
            emulate_tty=True,
        ),
    ])
