"""
Author: Orion
Date: 2025-05-19 16:28:23
LastEditors: Orion
LastEditTime: 2025-05-19 16:43:49
Description:

"""

import sys
import socket
import threading
import json
import webbrowser
import os
from PySide6.QtWidgets import (
    QApplication,
    QMainWindow,
    QWidget,
    QTableWidgetItem,
    QTableWidget,
    QAbstractItemView,
    QPushButton,
    QHBoxLayout,
    QVBoxLayout,
    QDialog,
    QLineEdit,
    QLabel,
    QVBoxLayout,
    QMessageBox,
    QHeaderView,
    QMenu,
)
from PySide6.QtCore import Qt, Signal, QObject
from PySide6.QtGui import QFont, QImage, QPixmap, QIcon, QClipboard

import scan_device_ui
import qdarktheme
import requests
import urllib.request
from PySide6.QtWidgets import QProgressDialog

VERSION = "1.2"


class LoginDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("输入用户名和密码")
        self.layout = QVBoxLayout(self)

        self.label_username = QLabel("用户名:", self)
        self.layout.addWidget(self.label_username)

        self.username = QLineEdit(self)
        self.layout.addWidget(self.username)

        self.label_password = QLabel("密码:", self)
        self.layout.addWidget(self.label_password)

        self.password = QLineEdit(self)
        self.password.setEchoMode(QLineEdit.Password)
        self.layout.addWidget(self.password)

        self.button_login = QPushButton("确定", self)
        self.button_login.clicked.connect(self.accept)
        self.layout.addWidget(self.button_login)

    def get_credentials(self):
        return self.username.text(), self.password.text()


class Signal(QObject):
    device_found = Signal(dict)


class QDeviceWidget(QWidget):
    def __init__(self):
        super().__init__()
        self.ui = scan_device_ui.Ui_Form()
        self.ui.setupUi(self)
        self.setWindowTitle(f"LK-设备发现 V{VERSION}")

        if hasattr(sys, "_MEIPASS"):
            base_path = sys._MEIPASS
        else:
            base_path = os.path.abspath(".")
        icon_path = os.path.join(base_path, "images", "lklogo.png")
        self.setWindowIcon(QIcon(icon_path))
        self.ui.scan_device.clicked.connect(self.scan_device)
        self.ui.reset_ip.clicked.connect(self.reset_ip_info)
        self.ui.exec_remote_cmd.clicked.connect(self.exec_remote_cmd)
        self.ui.et_remote_cmd.setText("device_info")
        self.UDP_PORT = 37210
        self.update_ui = Signal()
        self.update_ui.device_found.connect(self.update_list_ui)
        self.ui.update_button.clicked.connect(self.check_for_updates)

        self.init_tables()
        self.count_index = 0
        self.list_content = []
        self.selected_device_ip = None

    def init_tables(self):
        self.ui.device_list.setColumnCount(10)
        self.ui.device_list.setHorizontalHeaderLabels(
            [
                "设备类型",
                "设备名称",
                "操作",
                "以太网MAC",
                "WIFI MAC",
                "IP地址",
                "子网掩码",
                "网关",
                "版本",
                "网络类型",
            ]
        )
        self.ui.device_list.setSelectionBehavior(QAbstractItemView.SelectRows)
        header = self.ui.device_list.horizontalHeader()
        for i in range(9):
            header.setSectionResizeMode(i, QHeaderView.Interactive)
        header.setSectionResizeMode(9, QHeaderView.ResizeToContents)
        header.setStretchLastSection(True)
        self.ui.device_list.itemClicked.connect(self.table_item_click)
        self.ui.device_list.setContextMenuPolicy(Qt.CustomContextMenu)
        self.ui.device_list.customContextMenuRequested.connect(self.show_context_menu)

    def table_item_click(self, item):
        print("item click = ", item.row())
        content = self.list_content[item.row()]
        self.ui.et_name.setText(content[1])
        self.ui.et_ip.setText(content[4])
        self.ui.et_mask.setText(content[5])
        self.ui.et_gateway.setText(content[6])

        # 设置DNS字段
        self.ui.et_primary_dns.setText(
            content[9] if len(content) > 9 and content[9] else "8.8.8.8"
        )
        self.ui.et_secondary_dns.setText(
            content[10] if len(content) > 10 and content[10] else "114.114.114.114"
        )

        # 设置网络类型
        if len(content) > 8 and content[8] == "静态IP":
            self.ui.rb_static_ip.setChecked(True)
            self.ui.rb_dhcp_ip.setChecked(False)
        else:
            self.ui.rb_static_ip.setChecked(False)
            self.ui.rb_dhcp_ip.setChecked(True)

        self.selected_device_ip = content[4]

    def scan_device(self):
        print("scan device start ...")
        self.list_content = []
        self.count_index = 0
        self.ui.device_list.clearContents()
        self.ui.device_list.setRowCount(0)

        # 显示扫描状态
        self.ui.scan_device.setEnabled(False)
        self.ui.scan_device.setText("扫描中...")

        # 使用新线程进行扫描
        scan_thread = threading.Thread(target=self.scan_lan_with_status)
        scan_thread.daemon = True
        scan_thread.start()

    def scan_lan_with_status(self):
        try:
            self.scan_lan()
        finally:
            # 扫描完成后恢复按钮状态
            self.ui.scan_device.setEnabled(True)
            self.ui.scan_device.setText("搜索设备")

    def scan_lan(self):
        local_ip = self.get_local_ip()
        ip_parts = local_ip.split(".")
        subnet = ".".join(ip_parts[:3]) + "."

        # 创建线程列表，用于管理所有扫描线程
        threads = []

        # 保存已找到的设备IP，用于去重
        self.found_devices = set()

        for i in range(1, 255):
            ip = subnet + str(i)
            thread = threading.Thread(target=self.check_device, args=(ip,))
            threads.append(thread)
            thread.start()

            # 控制线程创建速度，避免过多线程同时创建
            if i % 10 == 0:
                threading.Event().wait(0.01)

        # 等待所有线程完成
        for thread in threads:
            thread.join(timeout=2.0)  # 设置超时时间，防止无响应线程阻塞

    def check_device(self, ip):
        # 尝试发送3次请求，提高发现率
        max_retries = 3
        for attempt in range(max_retries):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(1)
                try:
                    sock.sendto(b'{"device":"scan"}', (ip, self.UDP_PORT))
                    data, address = sock.recvfrom(1024)
                    json_data = json.loads(data.decode())
                    print(f"Device found: {json_data}")

                    # 检查是否已经添加过这个设备
                    device_ip = json_data.get("ip", "")
                    if device_ip and device_ip not in self.found_devices:
                        self.found_devices.add(device_ip)
                        self.update_ui.device_found.emit(json_data)

                    # 如果成功找到设备，不需要重试
                    return
                except socket.timeout:
                    # 超时后继续下一次尝试
                    continue
                except Exception as e:
                    # 其他错误，不再尝试
                    break

    def get_local_ip(self):
        # 获取本地机器的 IP 地址
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]

    def update_list_ui(self, data):
        print(f"Updating UI with data: {data}")
        self.ui.device_list.setRowCount(self.count_index + 1)

        item1 = QTableWidgetItem(data.get("type", ""))
        item1.setFlags(item1.flags() & ~Qt.ItemIsEditable)
        item1.setToolTip(item1.text())
        self.ui.device_list.setItem(self.count_index, 0, item1)

        item2 = QTableWidgetItem(data.get("name", ""))
        item2.setFlags(item2.flags() & ~Qt.ItemIsEditable)
        item2.setToolTip(item2.text())
        self.ui.device_list.setItem(self.count_index, 1, item2)

        eth_mac = data.get("eth_mac", data.get("mac", ""))
        item3 = QTableWidgetItem(eth_mac)
        item3.setFlags(item3.flags() & ~Qt.ItemIsEditable)
        item3.setToolTip(item3.text())
        self.ui.device_list.setItem(self.count_index, 3, item3)

        item4 = QTableWidgetItem(data.get("sta_mac", ""))
        item4.setFlags(item4.flags() & ~Qt.ItemIsEditable)
        item4.setToolTip(item4.text())
        self.ui.device_list.setItem(self.count_index, 4, item4)

        item5 = QTableWidgetItem(data.get("ip", ""))
        item5.setFlags(item5.flags() & ~Qt.ItemIsEditable)
        item5.setToolTip(item5.text())
        self.ui.device_list.setItem(self.count_index, 5, item5)

        item6 = QTableWidgetItem(data.get("netmask", ""))
        item6.setFlags(item6.flags() & ~Qt.ItemIsEditable)
        item6.setToolTip(item6.text())
        self.ui.device_list.setItem(self.count_index, 6, item6)

        item7 = QTableWidgetItem(data.get("gateway", ""))
        item7.setFlags(item7.flags() & ~Qt.ItemIsEditable)
        item7.setToolTip(item7.text())
        self.ui.device_list.setItem(self.count_index, 7, item7)

        item8 = QTableWidgetItem(data.get("version", ""))
        item8.setFlags(item8.flags() & ~Qt.ItemIsEditable)
        item8.setToolTip(item8.text())
        self.ui.device_list.setItem(self.count_index, 8, item8)

        # 添加网络类型（静态IP/动态IP）
        is_dhcp = data.get("is_dhcp", "1")
        network_type = "动态IP" if is_dhcp == "1" else "静态IP"
        item9 = QTableWidgetItem(network_type)
        item9.setFlags(item9.flags() & ~Qt.ItemIsEditable)
        item9.setToolTip(item9.text())
        self.ui.device_list.setItem(self.count_index, 9, item9)

        button = QPushButton("WEB")
        button.clicked.connect(lambda: self.open_browser(data.get("ip", "")))
        self.ui.device_list.setCellWidget(self.count_index, 2, button)

        # 更新列表内容，添加DNS和网络类型
        primary_dns = data.get("primary_dns", "8.8.8.8")
        secondary_dns = data.get("secondary_dns", "114.114.114.114")

        self.list_content.append(
            [
                data.get("type", ""),
                data.get("name", ""),
                eth_mac,
                data.get("sta_mac", ""),
                data.get("ip", ""),
                data.get("netmask", ""),
                data.get("gateway", ""),
                data.get("version", ""),
                network_type,
                primary_dns,
                secondary_dns,
            ]
        )
        self.count_index += 1

    def open_browser(self, ip):
        url = f"http://{ip}"
        webbrowser.open(url)

    def check_for_updates(self):
        try:
            response = requests.get("http://47.101.140.34:8080/qt/search/version.json")
            if response.status_code == 200:
                versions = response.json()
                current_version = VERSION

                latest_version = current_version
                latest_url = None

                for version_info in versions:
                    version_name = version_info["name"]
                    version_number = version_name.split("V")[-1].replace(".exe", "")
                    if float(version_number) > float(latest_version):
                        latest_version = version_number
                        latest_url = version_info["url"]

                if float(latest_version) > float(current_version):
                    reply = QMessageBox.question(
                        self,
                        "更新提示",
                        f"发现新版本 V{latest_version}，是否立即下载并更新？",
                        QMessageBox.Yes | QMessageBox.No,
                        QMessageBox.No,
                    )
                    if reply == QMessageBox.Yes:
                        self.download_and_update(latest_url)
                else:
                    QMessageBox.information(self, "提示", "当前已是最新版本")
            else:
                QMessageBox.warning(self, "错误", "无法获取版本信息")
        except Exception as e:
            QMessageBox.critical(self, "错误", f"发生错误: {str(e)}")

    def download_and_update(self, url):
        file_name = url.split("/")[-1]
        download_path = os.path.join(os.getcwd(), file_name)

        try:
            response = urllib.request.urlopen(url)
            file_size = int(response.getheader("Content-Length"))

            # 创建进度条对话框
            progress_dialog = QProgressDialog("下载中...", "取消", 0, 100, self)
            progress_dialog.setWindowTitle("下载更新")
            progress_dialog.setWindowModality(Qt.WindowModal)
            progress_dialog.setMinimumDuration(0)
            progress_dialog.setValue(0)

            block_size = 1024
            downloaded_size = 0

            with open(download_path, "wb") as out_file:
                while True:
                    buffer = response.read(block_size)
                    if not buffer:
                        break

                    downloaded_size += len(buffer)
                    out_file.write(buffer)

                    # 更新进度条
                    progress = int(downloaded_size * 100 / file_size)
                    progress_dialog.setValue(progress)

                    if progress_dialog.wasCanceled():
                        break

            # 下载完成后，重新命名文件并加上版本号
            version_number = url.split("/")[-1].split("V")[-1].replace(".exe", "")
            new_file_name = f"LK-设备发现工具V{version_number}.exe"
            new_download_path = os.path.join(os.getcwd(), new_file_name)

            if os.path.exists(new_download_path):
                os.remove(new_download_path)

            os.rename(download_path, new_download_path)
            if not progress_dialog.wasCanceled():
                QMessageBox.information(self, "更新", "下载完成，准备更新")
                self.perform_update(new_download_path)
            else:
                QMessageBox.warning(self, "更新取消", "下载已被取消")

        except Exception as e:
            QMessageBox.critical(self, "下载失败", f"下载过程中发生错误: {str(e)}")

    def perform_update(self, file_path):
        try:
            os.startfile(file_path)
            QMessageBox.information(self, "更新提示", "工具将关闭并启动升级程序")
            QApplication.quit()
        except Exception as e:
            QMessageBox.critical(self, "更新失败", f"启动更新程序时发生错误: {str(e)}")

    def reset_ip_info(self):
        self.authenticate_and_execute(self._reset_ip_info)

    def send_ip_info_to_backend(self):
        if self.selected_device_ip is None:
            self.show_error_message("请先选择一个设备")
            return False

        name = self.ui.et_name.text()
        ip = self.ui.et_ip.text()
        mask = self.ui.et_mask.text()
        gateway = self.ui.et_gateway.text()
        primary_dns = self.ui.et_primary_dns.text()
        secondary_dns = self.ui.et_secondary_dns.text()
        is_dhcp = "2" if self.ui.rb_static_ip.isChecked() else "1"

        parm = {
            "name": name,
            "ip": ip,
            "mask": mask,
            "gateway": gateway,
            "primary_dns": primary_dns,
            "secondary_dns": secondary_dns,
            "is_dhcp": is_dhcp,
        }
        cmd = json.dumps({"device": "set", "parm": parm})
        print("Sending IP info to backend: " + cmd)
        response = self.send_command_and_receive(cmd, self.selected_device_ip)
        if not response:
            self.show_error_message("设备没有响应")
            return False

        try:
            response_json = json.loads(response)
        except json.JSONDecodeError:
            self.show_error_message("设备返回异常响应")
            return False

        if response_json.get("change") == "ok":
            return True

        self.show_error_message(f"设置失败: {response}")
        return False

    def _reset_ip_info(self):
        if not self.send_ip_info_to_backend():
            return False

        reboot_cmd = json.dumps({"device": "set", "parm": {"command": "reboot"}})
        print("reboot param = " + reboot_cmd)
        response = self.send_command_and_receive(reboot_cmd, self.selected_device_ip)
        if not response:
            self.show_error_message("设备没有响应")
            return False

        try:
            response_json = json.loads(response)
        except json.JSONDecodeError:
            self.show_error_message("设备返回异常响应")
            return False

        if response_json.get("change") == "ok":
            return True

        self.show_error_message(f"重启失败: {response}")
        return False

    def exec_remote_cmd(self):
        if self.selected_device_ip is None:
            self.show_error_message("请先选择一个设备")
            return

        cmdstr = self.ui.et_remote_cmd.text()
        cmd = json.dumps({"device": "exec", "parm": cmdstr})
        print("exec_remote_cmd param = " + cmd)
        response = self.send_command_and_receive(cmd, self.selected_device_ip)

        if response:
            self.ui.et_cmd_result.setPlainText(response)
            try:
                response_json = json.loads(response)
                data = response_json.get("data", {})
                sys_info = response_json.get("sys", {})

                temperature = data.get("temperature")
                humidity = data.get("humidity")
                pressure = data.get("pressure")
                altitude = data.get("altitude")
                version = sys_info.get("version")

                result_text = (
                    f"温度: {temperature} ℃\n"
                    f"湿度: {humidity} %\n"
                    f"压力: {pressure} hPa\n"
                    f"海拔: {altitude} m\n"
                    f"版本: {version}"
                )

                self.ui.et_cmd_result.setPlainText(result_text)

            except json.JSONDecodeError:
                self.ui.et_cmd_result.setPlainText("正在升级中，请稍后重新搜索")
        else:
            self.ui.et_cmd_result.setPlainText("命令执行失败或无响应")

    def send_command_and_receive(self, cmd, ip):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(2)
            sock.sendto(cmd.encode(), (ip, self.UDP_PORT))
            try:
                data, _ = sock.recvfrom(1024)
                return data.decode()
            except socket.timeout:
                return None

    def authenticate_and_execute(self, action):
        username, password = self.show_login_dialog()
        if username and password:
            credentials = json.dumps({"username": username, "password": password})
            if self.selected_device_ip is None:
                self.show_error_message("请先选择一个设备")
                return
            ip = self.selected_device_ip
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(2)
                sock.sendto(credentials.encode(), (ip, self.UDP_PORT))
                try:
                    data, address = sock.recvfrom(1024)
                    response = json.loads(data.decode())
                    if response.get("result") == "ok":
                        if action and action():
                            self.show_success_message("设置成功，设备即将重启")
                    else:
                        self.show_error_message("认证失败，用户名或密码错误")
                except socket.timeout:
                    self.show_error_message("设备没有响应")
        else:
            self.show_error_message("取消操作")

    def show_login_dialog(self):
        dialog = LoginDialog(self)
        if dialog.exec() == QDialog.Accepted:
            username, password = dialog.get_credentials()
            return username, password
        return None, None

    def show_error_message(self, message):
        error_dialog = QDialog(self)
        error_dialog.setWindowTitle("错误")
        layout = QVBoxLayout(error_dialog)
        label = QLabel(message, error_dialog)
        layout.addWidget(label)
        button = QPushButton("确定", error_dialog)
        button.clicked.connect(error_dialog.accept)
        layout.addWidget(button)
        error_dialog.exec()

    def show_success_message(self, message):
        success_dialog = QDialog(self)
        success_dialog.setWindowTitle("成功")
        layout = QVBoxLayout(success_dialog)
        label = QLabel(message, success_dialog)
        layout.addWidget(label)
        button = QPushButton("确定", success_dialog)
        button.clicked.connect(success_dialog.accept)
        layout.addWidget(button)
        success_dialog.exec()

    def send_command(self, cmd, ip):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(cmd.encode(), (ip, self.UDP_PORT))

    def show_context_menu(self, position):
        # 获取右键点击位置的单元格
        item = self.ui.device_list.itemAt(position)
        if not item:
            return

        # 创建右键菜单
        context_menu = QMenu(self)
        copy_action = context_menu.addAction("复制")

        # 显示菜单
        action = context_menu.exec(self.ui.device_list.mapToGlobal(position))

        # 处理菜单选项
        if action == copy_action:
            self.copy_cell_content(item)

    def copy_cell_content(self, item):
        # 复制指定单元格的内容到剪贴板
        if item:
            clipboard = QApplication.clipboard()
            clipboard.setText(item.text())
            QMessageBox.information(self, "提示", "内容已复制到剪贴板")


if __name__ == "__main__":
    qdarktheme.enable_hi_dpi()
    app = QApplication(sys.argv)
    qdarktheme.setup_theme("light")
    qdarktheme.load_palette()
    w = QDeviceWidget()
    w.show()
    sys.exit(app.exec())
