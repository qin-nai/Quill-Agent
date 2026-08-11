import os
import subprocess
import sys

# ========== Windows 子进程隐藏窗口标志 ==========
CREATE_NO_WINDOW = 0x08000000

# ========== 路径兼容性处理 ==========
def get_base_dir():
    """获取程序运行根目录（兼容 PyInstaller 打包）"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    else:
        return os.path.dirname(os.path.abspath(__file__))

# 使用动态根目录
base_dir = get_base_dir()

# cmake 可执行文件路径
cmake_exe = os.path.join(base_dir, "cmake-4.4.2-windows-x86_64", "bin", "cmake.exe")

# 项目根目录
project_dir = os.path.join(base_dir, "Quill-Agent-main")

def build():
    """执行编译，返回 True 表示成功，False 表示失败"""
    print("开始编译...")
    print(f"[BUILD] 基础目录: {base_dir}")
    if not os.path.isfile(cmake_exe):
        print(f"错误：未找到 cmake.exe，路径：{cmake_exe}")
        return False
    if not os.path.isdir(project_dir):
        print(f"错误：项目目录不存在：{project_dir}")
        return False

    try:
        print("配置阶段...")
        subprocess.run(
            [cmake_exe, "-S", "core", "-B", "build"],
            cwd=project_dir,
            check=True,
            capture_output=False,
            creationflags=CREATE_NO_WINDOW
        )
        print("构建阶段...")
        subprocess.run(
            [cmake_exe, "--build", "build", "--config", "Release"],
            cwd=project_dir,
            check=True,
            creationflags=CREATE_NO_WINDOW
        )
        print("编译完成！")
        return True
    except subprocess.CalledProcessError as e:
        print(f"构建失败，错误码：{e.returncode}")
        return False

if __name__ == "__main__":
    success = build()
    sys.exit(0 if success else 1)