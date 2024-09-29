import taos
import sys
import os
import subprocess
import time
import random
import json
from datetime import datetime


###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-
dataDir = "/var/lib/taos/"
templateFile = "json/template.json"
Number = 0
resultContext = ""


def showLog(str):
    print(str)

def exec(command, show=True):
    if(show):
        print(f"exec {command}\n")
    return os.system(command)

# run return output and error
def run(command, timeout = 60, show=True):
    if(show):
        print(f"run {command} timeout={timeout}s\n")    

    process = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    process.wait(timeout)

    output = process.stdout.read().decode(encoding="gbk")
    error = process.stderr.read().decode(encoding="gbk")

    return output, error

# return list after run
def runRetList(command, timeout=10):
    output,error = run(command, timeout)
    return output.splitlines()

def readFileContext(filename):
    file = open(filename)
    context = file.read()
    file.close()
    return context
    

def writeFileContext(filename, context):
    file = open(filename, "w")
    file.write(context)
    file.close()

def appendFileContext(filename, context):
    global resultContext
    resultContext += context
    try:
        file = open(filename, "a")
        wsize = file.write(context)
        file.close()
    except:
        print(f"appand file error  context={context} .")

def getFolderSize(folder):
    total_size = 0
    for dirpath, dirnames, filenames in os.walk(folder):
        for filename in filenames:
            filepath = os.path.join(dirpath, filename)
            total_size += os.path.getsize(filepath)
    return total_size

def waitClusterAlive(loop):
    for i in range(loop):
        command = 'taos -s "show cluster alive\G;" '
        out,err = run(command)
        print(out)
        if out.find("status: 1") >= 0:
            showLog(f" i={i} wait cluster alive ok.\n")
            return True
        
        showLog(f" i={i} wait cluster alive ...\n")
        time.sleep(1)
    
    showLog(f" i={i} wait cluster alive failed.\n")
    return False

def waitCompactFinish(loop):
    for i in range(loop):
        command = 'taos -s "show compacts;" '
        out,err = run(command)
        if out.find("Query OK, 0 row(s) in set") >= 0:
            showLog(f" i={i} wait compact finish ok\n")
            return True
        
        showLog(f" i={i} wait compact ...\n")
        time.sleep(1)
    
    showLog(f" i={i} wait compact failed.\n")
    return False


def getTypeName(datatype):
    str1 = datatype.split(",")[0]
    str2 =     str1.split(":")[1]
    str3 = str2.replace('"','').replace(' ','')
    return str3


def getMatch(datatype, algo):
    if algo == "tsz":
        if datatype == "float" or datatype == "double":
            return True
        else:
            return False
    else:
        return True


def generateJsonFile(algo):
    print(f"doTest algo: {algo} \n")
    
    # replace datatype
    context = readFileContext(templateFile)
    # replace compress
    context = context.replace("@COMPRESS", algo)

    # write to file
    fileName = f"json/test_{algo}.json"
    if os.path.exists(fileName):
      os.remove(fileName)
    writeFileContext(fileName, context)

    return fileName

def taosdStart():
    cmd = "nohup /usr/bin/taosd 2>&1 & "
    ret = exec(cmd)
    print(f"exec taosd ret = {ret}\n")
    time.sleep(3)
    waitClusterAlive(10)

def taosdStop():
    i = 1
    toBeKilled = "taosd"
    killCmd = "ps -ef|grep -w %s| grep -v grep | awk '{print $2}' | xargs kill -TERM > /dev/null 2>&1" % toBeKilled
    psCmd   = "ps -ef|grep -w %s| grep -v grep | awk '{print $2}'" % toBeKilled
    processID = subprocess.check_output(psCmd, shell=True)
    while(processID):
        os.system(killCmd)
        time.sleep(1)
        processID = subprocess.check_output(psCmd, shell=True)
        print(f"i={i} kill taosd pid={processID}")
        i += 1

def cleanAndStartTaosd():
    
    # stop
    taosdStop()
    # clean
    exec(f"rm -rf {dataDir}")
    # start
    taosdStart()

def findContextValue(context, label):
    start = context.find(label)
    if start == -1 :
        return ""
    start += len(label) + 2
    # skip blank
    while context[start] == ' ':
        start += 1

    # find end ','
    end = start
    ends = [',','}',']', 0]
    while context[end] not in ends:
        end += 1

    print(f"start = {start} end={end}\n")
    return context[start:end]


def writeTemplateInfo(resultFile):
    # create info
    context = readFileContext(templateFile)
    vgroups    = findContextValue(context, "vgroups")
    childCount = findContextValue(context, "childtable_count")
    insertRows = findContextValue(context, "insert_rows")
    line = f"vgroups = {vgroups}\nchildtable_count = {childCount}\ninsert_rows = {insertRows}\n\n"
    print(line)
    appendFileContext(resultFile, line)


def totalCompressRate(algo, resultFile, writeSecond):
    global Number
    # flush
    command = 'taos -s "flush database dbrate;"'
    rets = exec(command)
    command = 'taos -s "compact database dbrate;"'
    rets = exec(command)
    waitCompactFinish(60)

    # read compress rate
    command = 'taos -s "show table distributed dbrate.meters\G;"'
    rets = runRetList(command)
    print(rets)

    str1 = rets[5]
    arr = str1.split(" ")

    # Total_Size KB
    str2 = arr[2]
    pos  = str2.find("=[")
    totalSize = int(float(str2[pos+2:])/1024)

    # Compression_Ratio
    str2 = arr[6]
    pos  = str2.find("=[")
    rate = str2[pos+2:]
    print("rate =" + rate)

    # total data file size
    #dataSize = getFolderSize(f"{dataDir}/vnode/")
    #dataSizeMB = int(dataSize/1024/1024)

    # appand to file
    
    Number += 1
    context =  "%10s %10s %10s %10s %10s\n"%( Number, algo, str(totalSize)+" MB", rate+"%", writeSecond + " s")
    showLog(context)
    appendFileContext(resultFile, context)


def doTest(algo, resultFile):
    print(f"doTest algo: {algo} \n")
    #cleanAndStartTaosd()


    # json
    jsonFile = generateJsonFile(algo)

    # run taosBenchmark
    t1 = time.time()
    exec(f"taosBenchmark -f {jsonFile}")
    t2 = time.time()

    # total compress rate
    totalCompressRate(algo, resultFile, str(int(t2-t1)))

def main():

    # test compress method
    algos = ["lz4", "zlib", "zstd", "xz", "disabled"]

    # record result
    resultFile = "./result.txt"
    timestamp = time.time()
    now = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(timestamp))
    context  = f"\n----------------------  test rate ({now}) ---------------------------------\n"

    appendFileContext(resultFile, context)
    # json info
    writeTemplateInfo(resultFile)
    # head
    context = "\n%10s %10s %10s %10s %10s\n"%("No", "compress", "dataSize", "rate", "insertSeconds")
    appendFileContext(resultFile, context)


    # loop for all compression
    for algo in algos:
        # do test
        doTest(algo, resultFile)
    appendFileContext(resultFile, "    \n")

    timestamp = time.time()
    now = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(timestamp))
    appendFileContext(resultFile, f"\n{now} finished test!\n")


if __name__ == "__main__":
    print("welcome use TDengine compress rate test tools.\n")
    main()
    # show result 
    print(resultContext)