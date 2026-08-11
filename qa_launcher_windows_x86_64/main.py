import sys
import os
import shutil
import subprocess
import time
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QToolButton, QMenu, QProgressBar, QMessageBox
)
from PyQt5.QtCore import Qt, QThread, pyqtSignal, QTimer, QProcess
from PyQt5.QtGui import QFont
from upset import build
import py7zr

# ========== Windows 子进程隐藏窗口标志 ==========
CREATE_NO_WINDOW = 0x08000000  # 仅 Windows 有效

# ========== 路径兼容性处理 ==========
def get_base_dir():
    """获取程序运行根目录（兼容 PyInstaller 打包）"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    else:
        return os.path.dirname(os.path.abspath(__file__))

# ========== 配置 ==========
GIT_REPO_URL = "https://github.com/qin-nai/Quill-Agent.git"
LOCAL_TOOLS_DIR = get_base_dir()

# ========== 便携版 Git 路径 ==========
def get_git_exe():
    git_path = os.path.join(LOCAL_TOOLS_DIR, "PortableGit", "bin", "git.exe")
    return git_path if os.path.isfile(git_path) else None

GIT_EXE = get_git_exe()

# ========== 工具函数 ==========
def make_writable(path):
    if not os.path.exists(path):
        return
    try:
        subprocess.run(
            ["attrib", "-r", path, "/s", "/d"],
            check=True, capture_output=True, timeout=30,
            creationflags=CREATE_NO_WINDOW  # 隐藏窗口
        )
        print(f"[DEBUG] 已移除只读属性: {path}")
    except Exception as e:
        print(f"[WARN] 移除只读属性失败 ({path}): {e}")

def extract_7z(archive_path, extract_to):
    print(f"[INFO] 开始解压 {archive_path} -> {extract_to}")
    with py7zr.SevenZipFile(archive_path, mode='r') as archive:
        archive.extractall(path=extract_to)
    make_writable(extract_to)
    print(f"[INFO] 解压完成")

def is_built():
    """检测是否已编译"""
    exe_path = os.path.join(LOCAL_TOOLS_DIR, "Quill-Agent-main", "build", "Release", "hermes_server.exe")
    return os.path.isfile(exe_path)

# ========== 启动器初始化 ==========
def init_local_tools():
    print("\n========== 初始化本地工具 ==========")
    print(f"[INFO] 基础目录: {LOCAL_TOOLS_DIR}")
    issues = []
    if GIT_EXE is None:
        issues.append("未找到 PortableGit，请将 PortableGit 文件夹放置于启动器同目录下")
    else:
        print(f"[OK] 便携版 Git: {GIT_EXE}")

    cmake_path = os.path.join(LOCAL_TOOLS_DIR, "cmake-4.4.2-windows-x86_64", "bin", "cmake.exe")
    if not os.path.isfile(cmake_path):
        issues.append("未找到 cmake，请将 cmake-4.4.2-windows-x86_64 文件夹放置于启动器同目录下")
    else:
        print(f"[OK] cmake: {cmake_path}")

    localgui_7z = os.path.join(LOCAL_TOOLS_DIR, "localgui.7z")
    localgui_dir = os.path.join(LOCAL_TOOLS_DIR, "localgui")
    if os.path.isfile(localgui_7z):
        print("[INFO] 发现 localgui.7z，正在解压...")
        try:
            if os.path.exists(localgui_dir):
                print("[INFO] 删除旧的 localgui 目录")
                make_writable(localgui_dir)
                shutil.rmtree(localgui_dir, ignore_errors=True)
            os.makedirs(localgui_dir, exist_ok=True)
            extract_7z(localgui_7z, localgui_dir)
            os.remove(localgui_7z)
            print("[INFO] localgui 解压完成，压缩包已删除")
        except Exception as e:
            issues.append(f"解压 localgui.7z 失败: {e}")
            print(f"[ERROR] {e}")
    else:
        if not os.path.isdir(localgui_dir):
            issues.append("未找到 localgui 目录或 localgui.7z，请确保客户端文件存在")
        else:
            print("[OK] localgui 目录已存在")

    if issues:
        print("[ERROR] 初始化失败: " + "\n".join(issues))
        return False, "\n".join(issues)
    print("[OK] 初始化完成")
    return True, "工具检查通过"


# ========== 源码验证线程 ==========
class VerifyThread(QThread):
    progress_signal = pyqtSignal(int, str)
    result_signal = pyqtSignal(int, str)

    def __init__(self):
        super().__init__()
        self.repo_url = GIT_REPO_URL
        self.root_dir_main = os.path.join(LOCAL_TOOLS_DIR, "Quill-Agent-main")
        self.git_exe = GIT_EXE
        self.last_error = ""

    def _git_command(self, args, cwd=None, timeout=60):
        if self.git_exe is None:
            return False, "", "Git 不可用"
        cmd = [self.git_exe] + args
        cmd_str = " ".join(cmd)
        cwd_str = cwd if cwd else os.getcwd()
        print(f"[GIT] 执行: {cmd_str}")
        print(f"[GIT] 工作目录: {cwd_str}")
        try:
            result = subprocess.run(
                cmd, cwd=cwd, check=False,
                capture_output=True, text=True, timeout=timeout,
                creationflags=CREATE_NO_WINDOW  # 隐藏窗口
            )
            print(f"[GIT] 返回码: {result.returncode}")
            if result.stdout:
                print(f"[GIT] stdout: {result.stdout[:200]}...")
            if result.stderr:
                print(f"[GIT] stderr: {result.stderr[:200]}...")
            return result.returncode == 0, result.stdout, result.stderr
        except subprocess.TimeoutExpired:
            print(f"[GIT ERROR] 命令超时 ({timeout}s): {cmd_str}")
            return False, "", "命令超时"
        except Exception as e:
            print(f"[GIT ERROR] 异常: {e}")
            return False, "", str(e)

    def _check_with_git(self):
        if not os.path.isdir(self.root_dir_main):
            return None, f"目录 {self.root_dir_main} 不存在"
        if not os.path.isdir(os.path.join(self.root_dir_main, ".git")):
            return None, "不是有效的 Git 仓库"

        self.progress_signal.emit(30, "正在获取远程更新...")
        print("[VERIFY] 开始执行 git fetch...")
        for attempt in range(2):
            ok, _, err = self._git_command(
                ["fetch", "origin", "main"],
                cwd=self.root_dir_main, timeout=60
            )
            if ok:
                print("[VERIFY] fetch 成功")
                break
            self.last_error = f"Fetch 失败 (尝试 {attempt+1}/2): {err}"
            print(f"[VERIFY] {self.last_error}")
            if attempt == 1:
                return None, self.last_error
            print("[VERIFY] 等待 2 秒后重试...")
            time.sleep(2)

        self.progress_signal.emit(60, "正在比对文件差异...")
        print("[VERIFY] 执行 git diff --quiet origin/main...")
        diff_ok, _, _ = self._git_command(
            ["diff", "--quiet", "origin/main"],
            cwd=self.root_dir_main, timeout=30
        )
        has_diff = not diff_ok
        if has_diff:
            print("[VERIFY] 检测到本地有修改")
        else:
            print("[VERIFY] 无差异")

        print("[VERIFY] 检查未跟踪文件...")
        _, untracked, _ = self._git_command(
            ["ls-files", "--others", "--exclude-standard"],
            cwd=self.root_dir_main, timeout=30
        )
        has_untracked = bool(untracked.strip())
        if has_untracked:
            print(f"[VERIFY] 发现未跟踪文件: {untracked.strip()}")

        if has_diff or has_untracked:
            return "update", "本地文件与远程不一致或有未跟踪文件"
        else:
            return "ok", "文件完整"

    def _clone_repo(self):
        if os.path.exists(self.root_dir_main):
            if os.path.isdir(os.path.join(self.root_dir_main, ".git")):
                print("[CLONE] 目录已存在且为 Git 仓库，跳过克隆")
                return True, "仓库已存在"
            print(f"[CLONE] 目标目录 {self.root_dir_main} 存在但不是 Git 仓库，尝试删除")
            make_writable(self.root_dir_main)
            try:
                subprocess.run(
                    ["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                    check=True, capture_output=True, timeout=30,
                    creationflags=CREATE_NO_WINDOW
                )
                print("[CLONE] 删除成功")
            except Exception as e:
                print(f"[CLONE] 删除失败，尝试重命名: {e}")
                backup = self.root_dir_main + ".old"
                if os.path.exists(backup):
                    shutil.rmtree(backup, ignore_errors=True)
                try:
                    os.rename(self.root_dir_main, backup)
                    print(f"[CLONE] 已重命名为 {backup}")
                except Exception as rename_err:
                    print(f"[CLONE] 重命名也失败: {rename_err}")
                    return False, f"无法清理目录: {rename_err}"

        cmd = [self.git_exe, "-c", "http.postBuffer=524288000",
               "clone", self.repo_url, self.root_dir_main]
        for attempt in range(3):
            try:
                self.progress_signal.emit(50 + attempt * 10, f"克隆尝试 {attempt+1}/3...")
                print(f"[CLONE] 尝试 {attempt+1}/3: {' '.join(cmd)}")
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1,
                    universal_newlines=True,
                    creationflags=CREATE_NO_WINDOW  # 隐藏窗口
                )
                start_time = time.time()
                while True:
                    line = process.stderr.readline()
                    if not line and process.poll() is not None:
                        break
                    if line:
                        print(f"[GIT-PROGRESS] {line.strip()}")
                    if time.time() - start_time > 600:
                        process.kill()
                        raise subprocess.TimeoutExpired(cmd, 600)
                returncode = process.wait()
                if returncode != 0:
                    remaining_err = process.stderr.read()
                    raise subprocess.CalledProcessError(returncode, cmd, stderr=remaining_err)
                make_writable(self.root_dir_main)
                print("[CLONE] 克隆成功")
                return True, "克隆成功"
            except subprocess.TimeoutExpired:
                print(f"[CLONE ERROR] 尝试 {attempt+1} 超时")
                if os.path.exists(self.root_dir_main):
                    make_writable(self.root_dir_main)
                    subprocess.run(["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                                   capture_output=True, creationflags=CREATE_NO_WINDOW)
                if attempt == 2:
                    return False, "克隆超时"
                time.sleep(3)
            except subprocess.CalledProcessError as e:
                print(f"[CLONE ERROR] 尝试 {attempt+1} 失败: {e.stderr}")
                if os.path.exists(self.root_dir_main):
                    make_writable(self.root_dir_main)
                    subprocess.run(["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                                   capture_output=True, creationflags=CREATE_NO_WINDOW)
                if attempt == 2:
                    return False, f"克隆失败: {e.stderr}"
                time.sleep(3)
        return False, "克隆失败，超过重试次数"

    def run(self):
        print("\n========== 开始源码完整性校验 ==========")
        self.progress_signal.emit(0, "开始验证源码...")

        if not os.path.exists(self.root_dir_main):
            self.progress_signal.emit(30, "本地无源码，开始下载...")
            success, msg = self._clone_repo()
            if success:
                self.progress_signal.emit(100, "源码下载完成")
                self.result_signal.emit(0, "")
            else:
                self.progress_signal.emit(100, f"下载失败: {msg}")
                self.result_signal.emit(3, msg)
            return

        if not os.path.isdir(os.path.join(self.root_dir_main, ".git")):
            self.progress_signal.emit(50, "目录存在但不是 Git 仓库，尝试重新克隆...")
            success, msg = self._clone_repo()
            if success:
                self.progress_signal.emit(100, "源码下载完成")
                self.result_signal.emit(0, "")
            else:
                self.progress_signal.emit(100, f"下载失败: {msg}")
                self.result_signal.emit(3, msg)
            return

        status, msg = self._check_with_git()
        if status is None:
            self.progress_signal.emit(100, f"检查失败: {msg}")
            self.result_signal.emit(3, msg)
            return

        if status == "ok":
            self.progress_signal.emit(100, "源码完整")
            self.result_signal.emit(0, "")
        else:
            self.progress_signal.emit(100, "源码需要更新")
            self.result_signal.emit(1, "")


# ========== 下载/更新源码线程 ==========
class DownloadThread(QThread):
    progress_signal = pyqtSignal(int, str)
    finished_signal = pyqtSignal(bool, str)

    def __init__(self, action="download", force=False):
        super().__init__()
        self.action = action
        self.force = force
        self.repo_url = GIT_REPO_URL
        self.root_dir_main = os.path.join(LOCAL_TOOLS_DIR, "Quill-Agent-main")
        self.git_exe = GIT_EXE

    def _clone_with_retry(self):
        if os.path.exists(self.root_dir_main):
            if os.path.isdir(os.path.join(self.root_dir_main, ".git")):
                print("[CLONE] 目录已存在且为 Git 仓库，转为 pull 操作")
                return self._pull_with_retry()
            print(f"[CLONE] 目标目录 {self.root_dir_main} 存在但不是 Git 仓库，尝试删除")
            make_writable(self.root_dir_main)
            try:
                subprocess.run(
                    ["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                    check=True, capture_output=True, timeout=30,
                    creationflags=CREATE_NO_WINDOW
                )
                print("[CLONE] 删除成功")
            except Exception as e:
                print(f"[CLONE] 删除失败，尝试重命名: {e}")
                backup = self.root_dir_main + ".old"
                if os.path.exists(backup):
                    shutil.rmtree(backup, ignore_errors=True)
                try:
                    os.rename(self.root_dir_main, backup)
                    print(f"[CLONE] 已重命名为 {backup}")
                except Exception as rename_err:
                    print(f"[CLONE] 重命名也失败: {rename_err}")
                    return False, f"无法清理目录: {rename_err}"

        cmd = [self.git_exe, "-c", "http.postBuffer=524288000",
               "clone", self.repo_url, self.root_dir_main]
        for attempt in range(3):
            try:
                self.progress_signal.emit(30 + attempt * 20, f"克隆尝试 {attempt+1}/3...")
                print(f"[CLONE] 尝试 {attempt+1}/3: {' '.join(cmd)}")
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    bufsize=1,
                    universal_newlines=True,
                    creationflags=CREATE_NO_WINDOW
                )
                start_time = time.time()
                while True:
                    line = process.stderr.readline()
                    if not line and process.poll() is not None:
                        break
                    if line:
                        print(f"[GIT-PROGRESS] {line.strip()}")
                    if time.time() - start_time > 600:
                        process.kill()
                        raise subprocess.TimeoutExpired(cmd, 600)
                returncode = process.wait()
                if returncode != 0:
                    remaining_err = process.stderr.read()
                    raise subprocess.CalledProcessError(returncode, cmd, stderr=remaining_err)
                make_writable(self.root_dir_main)
                print("[CLONE] 克隆成功")
                return True, "克隆成功"
            except subprocess.TimeoutExpired:
                print(f"[CLONE ERROR] 尝试 {attempt+1} 超时")
                if os.path.exists(self.root_dir_main):
                    make_writable(self.root_dir_main)
                    subprocess.run(["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                                   capture_output=True, creationflags=CREATE_NO_WINDOW)
                if attempt == 2:
                    return False, "克隆超时，多次重试失败"
                time.sleep(3)
            except subprocess.CalledProcessError as e:
                print(f"[CLONE ERROR] 尝试 {attempt+1} 失败: {e.stderr}")
                if os.path.exists(self.root_dir_main):
                    make_writable(self.root_dir_main)
                    subprocess.run(["cmd", "/c", "rd", "/s", "/q", self.root_dir_main],
                                   capture_output=True, creationflags=CREATE_NO_WINDOW)
                if attempt == 2:
                    return False, f"克隆失败: {e.stderr}"
                time.sleep(3)
        return False, "克隆失败，超过重试次数"

    def _pull_with_retry(self):
        if not os.path.exists(self.root_dir_main):
            return False, "项目目录不存在，无法更新"
        for attempt in range(3):
            try:
                self.progress_signal.emit(30 + attempt*20, f"拉取尝试 {attempt+1}/3...")
                if self.force:
                    print("[PULL] 强制更新模式：fetch --all + reset --hard origin/main")
                    subprocess.run([self.git_exe, "fetch", "--all"], cwd=self.root_dir_main,
                                   check=True, capture_output=True, text=True, timeout=60,
                                   creationflags=CREATE_NO_WINDOW)
                    subprocess.run([self.git_exe, "reset", "--hard", "origin/main"], cwd=self.root_dir_main,
                                   check=True, capture_output=True, text=True, timeout=60,
                                   creationflags=CREATE_NO_WINDOW)
                else:
                    print("[PULL] 常规更新：git pull")
                    subprocess.run([self.git_exe, "pull"], cwd=self.root_dir_main,
                                   check=True, capture_output=True, text=True, timeout=300,
                                   creationflags=CREATE_NO_WINDOW)
                make_writable(self.root_dir_main)
                print("[PULL] 更新成功")
                return True, "更新成功"
            except subprocess.TimeoutExpired:
                print(f"[PULL ERROR] 尝试 {attempt+1} 超时")
                if attempt == 2:
                    return False, "拉取超时，多次重试失败"
                time.sleep(3)
            except subprocess.CalledProcessError as e:
                print(f"[PULL ERROR] 尝试 {attempt+1} 失败: {e.stderr}")
                if attempt == 2:
                    return False, f"拉取失败: {e.stderr}"
                time.sleep(3)
        return False, "拉取失败，超过重试次数"

    def run(self):
        if self.action == "download":
            if os.path.exists(self.root_dir_main) and os.path.isdir(os.path.join(self.root_dir_main, ".git")):
                print("[DOWNLOAD] 检测到已存在的仓库，自动转为更新操作")
                self.action = "update"

        if self.action == "download":
            self.progress_signal.emit(0, "正在下载源码...")
            success, msg = self._clone_with_retry()
        else:
            self.progress_signal.emit(0, "正在更新源码...")
            success, msg = self._pull_with_retry()

        if success:
            self.progress_signal.emit(100, "源码操作完成")
        else:
            self.progress_signal.emit(100, f"源码操作失败: {msg}")
        self.finished_signal.emit(success, msg)


# ========== 编译线程 ==========
class BuildThread(QThread):
    finished_signal = pyqtSignal(bool)

    def run(self):
        print("[BUILD] 开始编译...")
        success = build()   # build() 内部已使用 CREATE_NO_WINDOW
        if success:
            print("[BUILD] 编译成功")
        else:
            print("[BUILD] 编译失败")
        self.finished_signal.emit(success)


# ========== 主窗口（保持不变，仅路径使用 LOCAL_TOOLS_DIR） ==========
class MainWindow(QMainWindow):
    # 此部分与先前完全相同，无需改动，但为保证完整性已全部复制
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Quill Agent Launcher")
        self.setFixedSize(600, 460)
        self.verify_thread = None
        self.download_thread = None
        self.build_thread = None
        self.init_thread = None
        self.verifying = False
        self.downloading = False
        self.building = False
        self.launcher_process = None
        self.launcher_running = False
        self.tools_ready = False

        self.hide_timer = QTimer(self)
        self.hide_timer.setSingleShot(True)
        self.hide_timer.timeout.connect(self._reset_ui)

        self.initUI()
        self.initSignals()
        self.start_initialization()

    def initUI(self):
        central = QWidget()
        self.setCentralWidget(central)
        central.setStyleSheet("background-color: #FFFFFF;")

        main_layout = QVBoxLayout(central)
        main_layout.setSpacing(20)
        main_layout.setContentsMargins(50, 50, 50, 10)
        main_layout.addStretch()

        self.title_label = QLabel("Quill Agent")
        self.title_label.setAlignment(Qt.AlignCenter)
        font = QFont("Microsoft YaHei", 30, QFont.Bold)
        font.setFamily("Microsoft YaHei, SimHei, sans-serif")
        self.title_label.setFont(font)
        self.title_label.setStyleSheet("color: #1A1C20;")
        main_layout.addWidget(self.title_label)
        main_layout.addSpacing(30)

        btn_layout = QHBoxLayout()
        btn_layout.setAlignment(Qt.AlignCenter)
        btn_layout.setSpacing(12)

        self.download_btn = QPushButton("正在初始化...")
        self.download_btn.setEnabled(False)
        self.download_btn.setFixedSize(200, 60)
        self.download_btn.setStyleSheet("""
            QPushButton {
                background-color: #E07B54;
                color: #1A110B;
                border: none;
                border-radius: 12px;
                font-weight: bold;
                font-size: 16px;
                font-family: "Microsoft YaHei", sans-serif;
            }
            QPushButton:hover {
                background-color: #D06A43;
            }
            QPushButton:pressed {
                background-color: #C45A33;
            }
            QPushButton:disabled {
                background-color: #B0A8A0;
                color: #6B6B6B;
            }
        """)
        btn_layout.addWidget(self.download_btn)

        self.menu_btn = QToolButton()
        self.menu_btn.setFixedSize(50, 50)
        self.menu_btn.setPopupMode(QToolButton.InstantPopup)
        self.menu_btn.setText("▼")
        self.menu_btn.setStyleSheet("""
            QToolButton {
                background-color: #F5F4F1;
                border: 1px solid #E5E4E0;
                border-radius: 12px;
                font-size: 20px;
                color: #1A1C20;
                font-family: "Microsoft YaHei", sans-serif;
            }
            QToolButton:hover {
                background-color: #E8E7E3;
                border-color: #D0CFCB;
            }
            QToolButton:pressed {
                background-color: #DCDAD6;
            }
            QToolButton:disabled {
                background-color: #F0EFEC;
                color: #B0A8A0;
            }
        """)
        menu = QMenu()
        self.action_verify = menu.addAction("验证源码完整性")
        self.action_repair = menu.addAction("修复源码")
        self.action_uninstall = menu.addAction("卸载")
        self.action_verify.setEnabled(False)
        self.action_repair.setEnabled(False)
        self.action_uninstall.setEnabled(False)
        self.menu_btn.setMenu(menu)
        btn_layout.addWidget(self.menu_btn)

        main_layout.addLayout(btn_layout)
        main_layout.addStretch()

        progress_layout = QVBoxLayout()
        progress_layout.setSpacing(6)
        progress_layout.setAlignment(Qt.AlignCenter)

        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setTextVisible(False)
        self.progress_bar.setStyleSheet("""
            QProgressBar {
                border: 1px solid #E5E4E0;
                border-radius: 8px;
                background: #F5F4F1;
                height: 20px;
            }
            QProgressBar::chunk {
                background: #E07B54;
                border-radius: 8px;
            }
        """)
        self.progress_bar.hide()
        progress_layout.addWidget(self.progress_bar)

        self.progress_label = QLabel("")
        self.progress_label.setAlignment(Qt.AlignCenter)
        self.progress_label.setStyleSheet("color: #6B7280; font-size: 14px; font-family: 'Microsoft YaHei', sans-serif;")
        self.progress_label.hide()
        progress_layout.addWidget(self.progress_label)

        main_layout.addLayout(progress_layout)
        main_layout.addStretch()

        bottom_layout = QHBoxLayout()
        bottom_layout.setContentsMargins(0, 0, 0, 0)
        copyright_label = QLabel("Copyright (c) 2026 Liu-Zhiyan")
        copyright_label.setAlignment(Qt.AlignCenter)
        copyright_label.setStyleSheet("color: #6B7280; font-size: 15px; font-family: 'Microsoft YaHei', sans-serif;")
        version_label = QLabel("v1.0")
        version_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        version_label.setStyleSheet("color: #6B7280; font-size: 15px; font-family: 'Microsoft YaHei', sans-serif;")
        bottom_layout.addStretch()
        bottom_layout.addWidget(copyright_label)
        bottom_layout.addStretch()
        bottom_layout.addWidget(version_label)
        main_layout.addLayout(bottom_layout)

    def initSignals(self):
        self.download_btn.clicked.connect(self.on_download_clicked)
        self.action_verify.triggered.connect(self.on_verify)
        self.action_repair.triggered.connect(self.on_repair)
        self.action_uninstall.triggered.connect(self.on_uninstall)

    def start_initialization(self):
        print("\n========== 启动器初始化 ==========")
        self.hide_timer.stop()
        self.download_btn.setEnabled(False)
        self.menu_btn.setEnabled(False)
        self.action_verify.setEnabled(False)
        self.action_repair.setEnabled(False)
        self.action_uninstall.setEnabled(False)

        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.show()
        self.progress_label.show()
        self.progress_label.setText("检查本地工具...")

        class InitThread(QThread):
            finished = pyqtSignal(bool, str)
            progress = pyqtSignal(int, str)
            def run(self):
                ok, msg = init_local_tools()
                self.progress.emit(100, "初始化完成")
                self.finished.emit(ok, msg)

        self.init_thread = InitThread()
        self.init_thread.progress.connect(self.update_progress)
        self.init_thread.finished.connect(self.on_init_finished)
        self.init_thread.start()

    def on_init_finished(self, success, msg):
        print(f"[INIT] 结果: {'成功' if success else '失败'}, 信息: {msg}")
        self.progress_bar.setRange(0, 100)
        if success:
            self.tools_ready = True
            self.progress_label.setText("工具就绪")
            self.start_verification()
        else:
            self.progress_label.setText(msg)
            self.download_btn.setText("工具缺失，请手动放置")
            self.download_btn.setEnabled(False)
            self.menu_btn.setEnabled(True)
            self.action_uninstall.setEnabled(True)
            self.hide_timer.start(5000)

    def start_verification(self):
        if self.verifying or self.downloading or self.building:
            return
        self.hide_timer.stop()
        self.verifying = True
        self.download_btn.setEnabled(False)
        self.menu_btn.setEnabled(False)
        self.action_verify.setEnabled(False)
        self.action_repair.setEnabled(False)
        self.action_uninstall.setEnabled(False)

        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.show()
        self.progress_label.show()
        self.progress_label.setText("开始验证源码...")

        print("[UI] 启动 VerifyThread")
        self.verify_thread = VerifyThread()
        self.verify_thread.progress_signal.connect(self.update_progress)
        self.verify_thread.result_signal.connect(self.on_verification_finished)
        self.verify_thread.start()

    def update_progress(self, value, text):
        self.progress_bar.setValue(value)
        self.progress_label.setText(text)

    def on_verification_finished(self, result_code, error_msg):
        print(f"[UI] 验证完成，结果码: {result_code}, 错误: {error_msg}")
        if result_code == 0:
            if is_built():
                btn_text = "启动服务"
                self.action_repair.setEnabled(True)
                self.progress_label.setText("源码完整且已编译")
            else:
                btn_text = "开始编译"
                self.action_repair.setEnabled(True)
                self.progress_label.setText("源码完整，需编译")
        elif result_code == 1:
            btn_text = "更新源码"
            self.action_repair.setEnabled(True)
            self.progress_label.setText("源码需要更新")
        else:
            btn_text = "验证失败，点击重试"
            self.action_repair.setEnabled(False)
            self.progress_label.setText(f"错误: {error_msg}" if error_msg else "验证失败")

        self.download_btn.setText(btn_text)
        self.download_btn.setEnabled(True)
        self.menu_btn.setEnabled(True)
        self.action_verify.setEnabled(True)
        self.action_uninstall.setEnabled(True)
        self.verifying = False
        self.hide_timer.start(5000)

    def start_download(self, action=None, force=False):
        if self.downloading or self.verifying or self.building:
            return
        self.hide_timer.stop()
        if action is None:
            text = self.download_btn.text()
            if text == "下载源码":
                action = "download"
            elif text == "更新源码":
                action = "update"
            else:
                action = "download"

        self.downloading = True
        self.download_btn.setEnabled(False)
        self.menu_btn.setEnabled(False)
        self.action_verify.setEnabled(False)
        self.action_repair.setEnabled(False)
        self.action_uninstall.setEnabled(False)

        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.show()
        self.progress_label.show()
        self.progress_label.setText("正在操作源码...")

        print(f"[UI] 启动 DownloadThread, action={action}, force={force}")
        self.download_thread = DownloadThread(action=action, force=force)
        self.download_thread.progress_signal.connect(self.update_progress)
        self.download_thread.finished_signal.connect(self.on_download_finished)
        self.download_thread.start()

    def on_download_finished(self, success, msg):
        print(f"[UI] 下载/更新完成，成功: {success}, 信息: {msg}")
        self.downloading = False
        if success:
            self.start_build()
        else:
            self.download_btn.setText("源码操作失败，点击重试")
            self.progress_label.setText(msg)
            self.action_repair.setEnabled(False)
            self.download_btn.setEnabled(True)
            self.menu_btn.setEnabled(True)
            self.action_verify.setEnabled(True)
            self.action_uninstall.setEnabled(True)
            self.hide_timer.start(5000)

    def start_build(self):
        if self.building or self.verifying or self.downloading:
            return
        self.hide_timer.stop()
        self.building = True
        self.download_btn.setEnabled(False)
        self.menu_btn.setEnabled(False)
        self.action_verify.setEnabled(False)
        self.action_repair.setEnabled(False)
        self.action_uninstall.setEnabled(False)

        self.progress_bar.setRange(0, 0)
        self.progress_bar.setValue(0)
        self.progress_bar.show()
        self.progress_label.show()
        self.progress_label.setText("编译中，可能需要一些时间")

        print("[UI] 启动 BuildThread")
        self.build_thread = BuildThread()
        self.build_thread.finished_signal.connect(self.on_build_finished)
        self.build_thread.start()

    def on_build_finished(self, success):
        print(f"[UI] 编译完成，成功: {success}")
        self.progress_bar.setRange(0, 100)
        if success:
            self.progress_bar.setValue(100)
            self.progress_label.setText("编译完成")
            self.download_btn.setText("启动服务")
        else:
            self.progress_bar.setValue(0)
            self.progress_label.setText("编译失败")
            self.download_btn.setText("编译失败，点击重试")

        self.download_btn.setEnabled(True)
        self.menu_btn.setEnabled(True)
        self.action_verify.setEnabled(True)
        self.action_repair.setEnabled(True)
        self.action_uninstall.setEnabled(True)
        self.building = False
        self.hide_timer.start(3000)

    def start_service(self):
        if self.launcher_running:
            return
        self.hide_timer.stop()
        launcher_path = os.path.join(LOCAL_TOOLS_DIR, "localgui", "QuillAgentLauncher.exe")
        if not os.path.exists(launcher_path):
            QMessageBox.warning(self, "错误", f"未找到启动器: {launcher_path}")
            return
        try:
            self.launcher_process = QProcess()
            self.launcher_process.start(launcher_path)
            if self.launcher_process.waitForStarted(3000):
                self.launcher_running = True
                self.download_btn.setEnabled(False)
                self.menu_btn.setEnabled(False)
                self.progress_label.setText("服务已启动")
                self.progress_label.show()
                self.launcher_process.finished.connect(self.on_launcher_finished)
            else:
                QMessageBox.warning(self, "错误", "启动服务失败")
        except Exception as e:
            QMessageBox.warning(self, "错误", f"启动服务异常: {e}")

    def on_launcher_finished(self):
        self.launcher_running = False
        self.download_btn.setEnabled(True)
        self.menu_btn.setEnabled(True)
        self.progress_label.setText("服务已退出")
        self.hide_timer.start(3000)

    def on_uninstall(self):
        reply = QMessageBox.question(
            self, "确认卸载",
            "确定要删除源代码目录吗？\n\n- .\\Quill-Agent-main\n\n此操作将删除所有源代码和编译产物，不可恢复！",
            QMessageBox.Yes | QMessageBox.No,
            QMessageBox.No
        )
        if reply != QMessageBox.Yes:
            return
        self.progress_bar.show()
        self.progress_label.show()
        self.progress_label.setText("正在卸载...")
        self.progress_bar.setRange(0, 0)

        dir_to_remove = os.path.join(LOCAL_TOOLS_DIR, "Quill-Agent-main")
        success = True
        if os.path.exists(dir_to_remove):
            make_writable(dir_to_remove)
            try:
                shutil.rmtree(dir_to_remove)
                print(f"已删除: {dir_to_remove}")
            except Exception as e:
                try:
                    subprocess.run(["cmd", "/c", "rd", "/s", "/q", dir_to_remove],
                                   check=True, capture_output=True, creationflags=CREATE_NO_WINDOW)
                    print(f"已强制删除: {dir_to_remove}")
                except Exception:
                    print(f"删除 {dir_to_remove} 失败: {e}")
                    success = False

        self.progress_bar.setRange(0, 100)
        if success:
            self.progress_bar.setValue(100)
            self.progress_label.setText("卸载完成")
            self.download_btn.setText("下载源码")
            self.action_repair.setEnabled(False)
        else:
            self.progress_bar.setValue(0)
            self.progress_label.setText("卸载失败，请检查权限")

        self.hide_timer.start(3000)

    def on_repair(self):
        self.start_download(action="update", force=True)

    def _reset_ui(self):
        if not self.launcher_running:
            self.progress_bar.hide()
            self.progress_label.hide()
        self.verify_thread = None
        self.download_thread = None
        self.init_thread = None

    def on_download_clicked(self):
        text = self.download_btn.text()
        if text == "启动服务":
            self.start_service()
        elif text == "下载源码":
            self.start_download(action="download", force=False)
        elif text == "更新源码":
            self.start_download(action="update", force=False)
        elif text == "开始编译":
            self.start_build()
        elif text == "编译失败，点击重试":
            self.start_build()
        elif text == "验证失败，点击重试":
            self.start_verification()
        elif text == "源码操作失败，点击重试":
            self.start_download()
        else:
            self.start_verification()

    def on_verify(self):
        self.start_verification()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())