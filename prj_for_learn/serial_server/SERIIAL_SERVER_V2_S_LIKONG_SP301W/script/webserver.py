'''
Author: Orion
Date: 2024-02-23 11:12:33
LastEditors: Orion
LastEditTime: 2024-04-18 18:17:32
FilePath: \ETH_TH\script\webserver.py
Description: 

Copyright (c) 2024 by SX-IOT, All Rights Reserved. 
'''
from flask import Flask, request, render_template,jsonify
import json

app = Flask(__name__)

# 创建一个字典来存储数据
data_store = {}

@app.route('/', methods=['GET', 'POST'])
def index():
    global data_store
    if request.method == 'POST':
        data = request.get_json()
        if data is None:
            print("Error: Data is None")
            return "Error", 500
        # 更新存储的数据
        data_store = data
        print(data)
    # return render_template('index.html', data=data_store)
    return jsonify(data_store)

if __name__ == "__main__":
    app.run(host='0.0.0.0', port=8080)