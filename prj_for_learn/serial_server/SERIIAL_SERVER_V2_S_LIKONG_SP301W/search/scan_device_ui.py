from PySide6 import QtCore, QtGui, QtWidgets

class Ui_Form(object):
    def setupUi(self, Form):
        Form.setObjectName("Form")
        Form.resize(1200, 650)
        self.verticalLayout_2 = QtWidgets.QVBoxLayout(Form)
        self.verticalLayout_2.setObjectName("verticalLayout_2")
        self.horizontalLayout = QtWidgets.QHBoxLayout()
        self.horizontalLayout.setContentsMargins(-1, 10, -1, 10)
        self.horizontalLayout.setObjectName("horizontalLayout")
        self.scan_device = QtWidgets.QPushButton(Form)
        self.scan_device.setObjectName("scan_device")
        self.horizontalLayout.addWidget(self.scan_device)

        # 在线升级按钮直接放在搜索设备按钮旁边
        self.update_button = QtWidgets.QPushButton(Form)
        self.update_button.setObjectName("update_button")
        self.horizontalLayout.addWidget(self.update_button)

        spacerItem = QtWidgets.QSpacerItem(40, 20, QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Minimum)
        self.horizontalLayout.addItem(spacerItem)

        self.verticalLayout_2.addLayout(self.horizontalLayout)
        self.horizontalLayout_3 = QtWidgets.QHBoxLayout()
        self.horizontalLayout_3.setObjectName("horizontalLayout_3")
        self.device_list = QtWidgets.QTableWidget(Form)
        self.device_list.setObjectName("device_list")
        self.device_list.setColumnCount(9)
        self.device_list.setRowCount(0)
        self.device_list.setHorizontalHeaderLabels([
            '设备类型', '设备名称', '操作','以太网MAC', 'WIFI MAC', 'IP地址',
            '子网掩码', '网关', '版本'
        ])
        self.device_list.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        header = self.device_list.horizontalHeader()
        header.setSectionResizeMode(QtWidgets.QHeaderView.Interactive)
        header.setStretchLastSection(True)
        self.device_list.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOn)
        self.horizontalLayout_3.addWidget(self.device_list)

        # 添加右侧设置区域的容器
        self.settingsContainer = QtWidgets.QWidget(Form)
        self.settingsContainer.setObjectName("settingsContainer")
        self.settingsContainer.setMaximumWidth(280) # 设置最大宽度
        self.settingsLayout = QtWidgets.QVBoxLayout(self.settingsContainer)
        self.settingsLayout.setContentsMargins(0, 0, 0, 0)
        self.settingsLayout.setSpacing(8) # 减小垂直间距
        self.settingsLayout.setObjectName("settingsLayout")

        # 网络设置分组框 - 调整顶部边距
        self.networkSettingsGroup = QtWidgets.QGroupBox("网络设置")
        self.networkSettingsGroup.setObjectName("networkSettingsGroup")
        self.networkSettingsGroup.setStyleSheet("QGroupBox { margin-top: 10px; }") # 减小顶部边距
        self.networkSettingsLayout = QtWidgets.QVBoxLayout(self.networkSettingsGroup)
        self.networkSettingsLayout.setObjectName("networkSettingsLayout")
        self.networkSettingsLayout.setContentsMargins(10, 15, 10, 10) # 减小内部边距

        # 将FormLayout放入分组框
        self.formLayout = QtWidgets.QFormLayout()
        self.formLayout.setObjectName("formLayout")
        # 设置FormLayout的字段增长策略，使标签占用最小空间
        self.formLayout.setFieldGrowthPolicy(QtWidgets.QFormLayout.AllNonFixedFieldsGrow)
        # 设置标签对齐方式为右对齐
        self.formLayout.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        # 设置水平间距更小
        self.formLayout.setHorizontalSpacing(8)
        # 设置垂直间距更小
        self.formLayout.setVerticalSpacing(6)
        # 设置表单布局的内边距
        self.formLayout.setContentsMargins(5, 5, 5, 5)

        self.label_name = QtWidgets.QLabel(Form)
        self.label_name.setObjectName("label_name")
        self.formLayout.setWidget(0, QtWidgets.QFormLayout.LabelRole, self.label_name)
        self.et_name = QtWidgets.QLineEdit(Form)
        self.et_name.setObjectName("et_name")
        self.et_name.setMaximumWidth(200)
        self.formLayout.setWidget(0, QtWidgets.QFormLayout.FieldRole, self.et_name)

        self.label_ip_type = QtWidgets.QLabel(Form)
        self.label_ip_type.setObjectName("label_ip_type")
        self.formLayout.setWidget(1, QtWidgets.QFormLayout.LabelRole, self.label_ip_type)

        self.ip_type_layout = QtWidgets.QHBoxLayout()
        self.ip_type_layout.setObjectName("ip_type_layout")

        self.rb_dhcp = QtWidgets.QRadioButton(Form)
        self.rb_dhcp.setObjectName("rb_dhcp")
        self.ip_type_layout.addWidget(self.rb_dhcp)

        self.rb_static = QtWidgets.QRadioButton(Form)
        self.rb_static.setObjectName("rb_static")
        self.rb_static.setChecked(True)
        self.ip_type_layout.addWidget(self.rb_static)

        self.formLayout.setLayout(1, QtWidgets.QFormLayout.FieldRole, self.ip_type_layout)

        self.label_ip = QtWidgets.QLabel(Form)
        self.label_ip.setObjectName("label_ip")
        self.formLayout.setWidget(2, QtWidgets.QFormLayout.LabelRole, self.label_ip)
        self.et_ip = QtWidgets.QLineEdit(Form)
        self.et_ip.setObjectName("et_ip")
        self.et_ip.setMaximumWidth(200)
        self.formLayout.setWidget(2, QtWidgets.QFormLayout.FieldRole, self.et_ip)

        self.label_mask = QtWidgets.QLabel(Form)
        self.label_mask.setObjectName("label_mask")
        self.formLayout.setWidget(3, QtWidgets.QFormLayout.LabelRole, self.label_mask)
        self.et_mask = QtWidgets.QLineEdit(Form)
        self.et_mask.setObjectName("et_mask")
        self.et_mask.setMaximumWidth(200)
        self.formLayout.setWidget(3, QtWidgets.QFormLayout.FieldRole, self.et_mask)

        self.label_gateway = QtWidgets.QLabel(Form)
        self.label_gateway.setObjectName("label_gateway")
        self.formLayout.setWidget(4, QtWidgets.QFormLayout.LabelRole, self.label_gateway)
        self.et_gateway = QtWidgets.QLineEdit(Form)
        self.et_gateway.setObjectName("et_gateway")
        self.et_gateway.setMaximumWidth(200)
        self.formLayout.setWidget(4, QtWidgets.QFormLayout.FieldRole, self.et_gateway)

        self.label_dns1 = QtWidgets.QLabel(Form)
        self.label_dns1.setObjectName("label_dns1")
        self.formLayout.setWidget(5, QtWidgets.QFormLayout.LabelRole, self.label_dns1)
        self.et_dns1 = QtWidgets.QLineEdit(Form)
        self.et_dns1.setObjectName("et_dns1")
        self.et_dns1.setMaximumWidth(200)
        self.formLayout.setWidget(5, QtWidgets.QFormLayout.FieldRole, self.et_dns1)

        self.label_dns2 = QtWidgets.QLabel(Form)
        self.label_dns2.setObjectName("label_dns2")
        self.formLayout.setWidget(6, QtWidgets.QFormLayout.LabelRole, self.label_dns2)
        self.et_dns2 = QtWidgets.QLineEdit(Form)
        self.et_dns2.setObjectName("et_dns2")
        self.et_dns2.setMaximumWidth(200)
        self.formLayout.setWidget(6, QtWidgets.QFormLayout.FieldRole, self.et_dns2)

        self.reset_ip = QtWidgets.QPushButton(Form)
        self.reset_ip.setObjectName("reset_ip")
        self.formLayout.setWidget(7, QtWidgets.QFormLayout.FieldRole, self.reset_ip)

        # 添加到网络设置分组框
        self.networkSettingsLayout.addLayout(self.formLayout)
        self.settingsLayout.addWidget(self.networkSettingsGroup)

        # 远程命令分组框
        self.remoteCmdGroup = QtWidgets.QGroupBox("远程命令")
        self.remoteCmdGroup.setObjectName("remoteCmdGroup")
        self.remoteCmdGroup.setStyleSheet("QGroupBox { margin-top: 10px; }") # 减小顶部边距
        self.remoteCmdLayout = QtWidgets.QVBoxLayout(self.remoteCmdGroup)
        self.remoteCmdLayout.setObjectName("remoteCmdLayout")
        self.remoteCmdLayout.setContentsMargins(10, 15, 10, 10) # 减小内部边距
        self.remoteCmdLayout.setSpacing(5) # 减小内部控件间的间距

        # 创建远程命令表单
        self.remoteCmdForm = QtWidgets.QFormLayout()
        self.remoteCmdForm.setObjectName("remoteCmdForm")
        self.remoteCmdForm.setFieldGrowthPolicy(QtWidgets.QFormLayout.AllNonFixedFieldsGrow)
        self.remoteCmdForm.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        self.remoteCmdForm.setHorizontalSpacing(8)
        self.remoteCmdForm.setVerticalSpacing(6)
        self.remoteCmdForm.setContentsMargins(5, 5, 5, 5)

        self.label_cmd = QtWidgets.QLabel(Form)
        self.label_cmd.setObjectName("label_cmd")
        self.remoteCmdForm.setWidget(0, QtWidgets.QFormLayout.LabelRole, self.label_cmd)
        self.et_remote_cmd = QtWidgets.QLineEdit(Form)
        self.et_remote_cmd.setObjectName("et_remote_cmd")
        # 为远程命令输入框设置最大宽度
        self.et_remote_cmd.setMaximumWidth(200)
        self.remoteCmdForm.setWidget(0, QtWidgets.QFormLayout.FieldRole, self.et_remote_cmd)

        self.exec_remote_cmd = QtWidgets.QPushButton(Form)
        self.exec_remote_cmd.setObjectName("exec_remote_cmd")
        self.remoteCmdForm.setWidget(1, QtWidgets.QFormLayout.FieldRole, self.exec_remote_cmd)

        self.et_cmd_result = QtWidgets.QTextEdit(Form)
        self.et_cmd_result.setObjectName("et_cmd_result")
        # 设置固定高度，避免命令结果区域占用过多空间
        self.et_cmd_result.setMaximumHeight(100)
        self.remoteCmdForm.setWidget(2, QtWidgets.QFormLayout.FieldRole, self.et_cmd_result)

        # 添加到远程命令分组框
        self.remoteCmdLayout.addLayout(self.remoteCmdForm)
        self.settingsLayout.addWidget(self.remoteCmdGroup)

        # 添加弹性空间，让分组框占据顶部位置
        spacerItem1 = QtWidgets.QSpacerItem(20, 40, QtWidgets.QSizePolicy.Minimum, QtWidgets.QSizePolicy.Expanding)
        self.settingsLayout.addItem(spacerItem1)

        self.horizontalLayout_3.addWidget(self.settingsContainer)
        # 修改左右区域比例为3:1
        self.horizontalLayout_3.setStretch(0, 3)
        self.horizontalLayout_3.setStretch(1, 1)
        self.verticalLayout_2.addLayout(self.horizontalLayout_3)

        self.horizontalLayout_4 = QtWidgets.QHBoxLayout()
        self.horizontalLayout_4.setObjectName("horizontalLayout_4")
        self.label_taobao = QtWidgets.QLabel(Form)
        self.label_taobao.setObjectName("label_taobao")
        self.label_taobao.setText('<a href="https://www.likong-iot.com">立控官网</a>')
        self.label_taobao.setOpenExternalLinks(True)
        self.horizontalLayout_4.addWidget(self.label_taobao)
        self.label_baidu = QtWidgets.QLabel(Form)
        self.label_baidu.setObjectName("label_baidu")
        self.label_baidu.setText('<a href="https://likongdianzi.taobao.com/">产品购买</a>')
        self.label_baidu.setOpenExternalLinks(True)
        self.horizontalLayout_4.addWidget(self.label_baidu)

        self.label_web_debug = QtWidgets.QLabel(Form)
        self.label_web_debug.setObjectName("label_web_debug")
        self.label_web_debug.setText('<a href="https://debug.likong-iot.com">Web调试</a>')
        self.label_web_debug.setOpenExternalLinks(True)
        self.label_web_debug.setStyleSheet("QLabel { text-decoration: none; }")
        self.horizontalLayout_4.addWidget(self.label_web_debug)

        self.label_doc = QtWidgets.QLabel(Form)
        self.label_doc.setObjectName("label_doc")
        self.label_doc.setText('<a href="https://docv2.likong-iot.com">接入文档</a>')
        self.label_doc.setOpenExternalLinks(True)
        self.label_doc.setStyleSheet("QLabel { text-decoration: none; }")
        self.horizontalLayout_4.addWidget(self.label_doc)


        spacerItem3 = QtWidgets.QSpacerItem(40, 20, QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Minimum)
        self.horizontalLayout_4.addItem(spacerItem3)
        self.verticalLayout_2.addLayout(self.horizontalLayout_4)

        self.verticalLayout_2.setStretch(1, 1)

        self.retranslateUi(Form)
        QtCore.QMetaObject.connectSlotsByName(Form)

    def retranslateUi(self, Form):
        _translate = QtCore.QCoreApplication.translate
        Form.setWindowTitle(_translate("Form", "Form"))
        self.scan_device.setText(_translate("Form", "搜索设备"))
        self.update_button.setText(_translate("Form", "在线升级"))
        self.label_name.setText(_translate("Form", "设备名称:"))
        self.label_ip_type.setText(_translate("Form", "网络类型:"))
        self.rb_dhcp.setText(_translate("Form", "动态IP"))
        self.rb_static.setText(_translate("Form", "静态IP"))
        self.label_ip.setText(_translate("Form", "IP地址:"))
        self.label_mask.setText(_translate("Form", "子网掩码:"))
        self.label_gateway.setText(_translate("Form", "网关地址:"))
        self.label_dns1.setText(_translate("Form", "首选DNS:"))
        self.label_dns2.setText(_translate("Form", "备选DNS:"))
        self.reset_ip.setText(_translate("Form", "重新设置"))
        self.label_cmd.setText(_translate("Form", "远程命令:"))
        self.exec_remote_cmd.setText(_translate("Form", "执行远程命令"))
