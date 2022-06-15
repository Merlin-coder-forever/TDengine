from ssl import ALERT_DESCRIPTION_CERTIFICATE_UNOBTAINABLE
import taos
import sys
import time
import os 

from util.log import *
from util.sql import *
from util.cases import *
from util.dnodes import TDDnodes
from util.dnodes import TDDnode
import time
import socket
import subprocess
from multiprocessing import Process
class MyDnodes(TDDnodes):
    def __init__(self ,dnodes_lists):
        super(MyDnodes,self).__init__()
        self.dnodes = dnodes_lists  # dnode must be TDDnode instance
        self.simDeployed = False
        
class TDTestCase:

    def init(self,conn ,logSql):
        tdLog.debug(f"start to excute {__file__}")
        self.TDDnodes = None

    def buildcluster(self,dnodenumber):
        self.depoly_cluster(dnodenumber)
        self.master_dnode = self.TDDnodes.dnodes[0]
        self.host=self.master_dnode.cfgDict["fqdn"]

        conn1 = taos.connect(self.master_dnode.cfgDict["fqdn"] , config=self.master_dnode.cfgDir)
        tdSql.init(conn1.cursor())
        

    def getBuildPath(self):
        selfPath = os.path.dirname(os.path.realpath(__file__))

        if ("community" in selfPath):
            projPath = selfPath[:selfPath.find("community")]
        else:
            projPath = selfPath[:selfPath.find("tests")]

        for root, dirs, files in os.walk(projPath):
            if ("taosd" in files):
                rootRealPath = os.path.dirname(os.path.realpath(root))
                if ("packaging" not in rootRealPath):
                    buildPath = root[:len(root) - len("/build/bin")]
                    break
        return buildPath
    
    def insert_data(self,count):
        # fisrt add data : db\stable\childtable\general table
        for couti in count:
            tdSql.execute("drop database if exists db%d" %couti)
            tdSql.execute("create database if not exists db%d replica 1 days 300" %couti)
            tdSql.execute("use db%d" %couti)
            tdSql.execute(
            '''create table stb1
            (ts timestamp, c1 int, c2 bigint, c3 smallint, c4 tinyint, c5 float, c6 double, c7 bool, c8 binary(16),c9 nchar(32), c10 timestamp)
            tags (t1 int)
            '''
            )
            tdSql.execute(
                '''
                create table t1
                (ts timestamp, c1 int, c2 bigint, c3 smallint, c4 tinyint, c5 float, c6 double, c7 bool, c8 binary(16),c9 nchar(32), c10 timestamp)
                '''
            )
            for i in range(4):
                tdSql.execute(f'create table ct{i+1} using stb1 tags ( {i+1} )')

    def depoly_cluster(self ,dnodes_nums): 

        testCluster = False
        valgrind = 0  
        hostname = socket.gethostname()
        dnodes = []
        start_port = 6030
        for num in range(1, dnodes_nums+1):
            dnode = TDDnode(num)
            dnode.addExtraCfg("firstEp", f"{hostname}:{start_port}")
            dnode.addExtraCfg("fqdn", f"{hostname}")
            dnode.addExtraCfg("serverPort", f"{start_port + (num-1)*100}")
            dnode.addExtraCfg("monitorFqdn", hostname)
            dnode.addExtraCfg("monitorPort", 7043)
            dnodes.append(dnode)
        
        self.TDDnodes = MyDnodes(dnodes)
        self.TDDnodes.init("")
        self.TDDnodes.setTestCluster(testCluster)
        self.TDDnodes.setValgrind(valgrind)
        self.TDDnodes.stopAll()
        for dnode in self.TDDnodes.dnodes:
            self.TDDnodes.deploy(dnode.index,{})
            
        for dnode in self.TDDnodes.dnodes:
            self.TDDnodes.starttaosd(dnode.index)

        # create cluster 
        for dnode in self.TDDnodes.dnodes[1:]:
            # print(dnode.cfgDict)
            dnode_id = dnode.cfgDict["fqdn"] +  ":" +dnode.cfgDict["serverPort"]
            dnode_first_host = dnode.cfgDict["firstEp"].split(":")[0]
            dnode_first_port = dnode.cfgDict["firstEp"].split(":")[-1]
            cmd = f" taos -h {dnode_first_host} -P {dnode_first_port} -s ' create dnode \"{dnode_id} \" ' ;"
            print(cmd)
            os.system(cmd)
        
        time.sleep(2)
        tdLog.info(" create cluster with %d dnode  done! " %dnodes_nums)

    def check3mnode(self):
        count=0
        while count < 10:
            time.sleep(1)
            tdSql.query("show mnodes;")
            if tdSql.checkRows(3) :                
                if  tdSql.queryResult[0][2]=='leader' :
                    if  tdSql.queryResult[1][2]=='follower':
                        if  tdSql.queryResult[2][2]=='follower':
                            print("three mnodes is ready in 10s")
                            break
                elif tdSql.queryResult[0][2]=='follower' :
                    if  tdSql.queryResult[1][2]=='leader':
                        if  tdSql.queryResult[2][2]=='follower':
                            print("three mnodes is ready in 10s")
                            break      
                elif tdSql.queryResult[0][2]=='follower' :
                    if  tdSql.queryResult[1][2]=='follower':
                        if  tdSql.queryResult[2][2]=='leader':
                            print("three mnodes is ready in 10s")
                            break                   
            count+=1
        else:
            print("three mnodes is not ready in 10s ")

        tdSql.query("show mnodes;")       
        tdSql.checkRows(3) 
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(0,3,'ready')
        tdSql.checkData(1,1,'chenhaoran02:6130')
        tdSql.checkData(1,3,'ready')
        tdSql.checkData(2,1,'chenhaoran02:6230')
        tdSql.checkData(2,3,'ready')

    def check3mnode1off(self):
        count=0
        while count < 10:
            time.sleep(1)
            tdSql.query("show mnodes;")
            if tdSql.checkRows(3) :
                if  tdSql.queryResult[0][2]=='offline' :
                    if  tdSql.queryResult[1][2]=='leader':
                        if  tdSql.queryResult[2][2]=='follower':
                            print("stop mnodes  on dnode 2 successfully in 10s")
                            break
                    elif tdSql.queryResult[1][2]=='follower':
                        if  tdSql.queryResult[2][2]=='leader':
                            print("stop mnodes  on dnode 2 successfully in 10s")
                            break
            count+=1
        else:
            print("stop mnodes  on dnode 2 failed in 10s ")

        tdSql.query("show mnodes;")       
        tdSql.checkRows(3) 
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(0,2,'offline')
        tdSql.checkData(0,3,'ready')
        tdSql.checkData(1,1,'chenhaoran02:6130')
        tdSql.checkData(1,3,'ready')
        tdSql.checkData(2,1,'chenhaoran02:6230')
        tdSql.checkData(2,3,'ready')

    def check3mnode2off(self):
        count=0
        while count < 10:
            time.sleep(1)
            tdSql.query("show mnodes;")
            if tdSql.checkRows(3) :
                if  tdSql.queryResult[0][2]=='leader' :
                    if  tdSql.queryResult[1][2]=='offline':
                        if  tdSql.queryResult[2][2]=='follower':
                            print("stop mnodes  on dnode 2 successfully in 10s")
                            break
            count+=1
        else:
            print("stop mnodes  on dnode 2 failed in 10s ")

        tdSql.query("show mnodes;")       
        tdSql.checkRows(3) 
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(0,2,'leader')
        tdSql.checkData(0,3,'ready')
        tdSql.checkData(1,1,'chenhaoran02:6130')
        tdSql.checkData(1,2,'offline')
        tdSql.checkData(1,3,'ready')
        tdSql.checkData(2,1,'chenhaoran02:6230')
        tdSql.checkData(2,2,'follower')
        tdSql.checkData(2,3,'ready')

    def check3mnode3off(self):
        count=0
        while count < 10:
            time.sleep(1)
            tdSql.query("show mnodes;")
            if tdSql.checkRows(3) :
                if  tdSql.queryResult[0][2]=='leader' :
                    if  tdSql.queryResult[2][2]=='offline':
                        if  tdSql.queryResult[1][2]=='follower':
                            print("stop mnodes  on dnode 3 successfully in 10s")
                            break
            count+=1
        else:
            print("stop mnodes  on dnode 3 failed in 10s")
            
        tdSql.query("show mnodes;")       
        tdSql.checkRows(3) 
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(0,2,'leader')
        tdSql.checkData(0,3,'ready')
        tdSql.checkData(1,1,'chenhaoran02:6130')
        tdSql.checkData(1,2,'follower')
        tdSql.checkData(1,3,'ready')
        tdSql.checkData(2,1,'chenhaoran02:6230')
        tdSql.checkData(2,2,'offline')
        tdSql.checkData(2,3,'ready')



    def five_dnode_three_mnode(self,dnodenumber):
        tdSql.query("show dnodes;")
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(4,1,'chenhaoran02:6430')
        tdSql.checkData(0,4,'ready')
        tdSql.checkData(4,4,'ready')
        tdSql.query("show mnodes;")   
        tdSql.checkRows(1)    
        tdSql.checkData(0,1,'chenhaoran02:6030')
        tdSql.checkData(0,2,'leader')
        tdSql.checkData(0,3,'ready')

        # fisr add three mnodes;
        tdSql.execute("create mnode on dnode 2")
        tdSql.execute("create mnode on dnode 3")

        # fisrt check statut ready
        self.check3mnode()


        tdSql.error("create mnode on dnode 2")

        tdSql.query("show dnodes;")
        print(tdSql.queryResult)
        #  stop and follower of mnode 
        stopcount =0 
        while stopcount <= 2:
            for i in range(dnodenumber):
                self.TDDnodes.stoptaosd(i+1)
                # if i == 1 :
                #     self.check3mnode2off()
                # elif i == 2 :
                #     self.check3mnode3off()
                # elif i == 0:
                #     self.check3mnode1off()

                self.TDDnodes.starttaosd(i+1)
                # self.check3mnode()
            stopcount+=1
        self.check3mnode()


    def getConnection(self, dnode):
        host = dnode.cfgDict["fqdn"]
        port = dnode.cfgDict["serverPort"]
        config_dir = dnode.cfgDir
        return taos.connect(host=host, port=int(port), config=config_dir)


    def run(self): 
        # print(self.master_dnode.cfgDict)
        self.buildcluster(5)
        self.five_dnode_three_mnode(5)

    def stop(self):
        tdSql.close()
        tdLog.success(f"{__file__} successfully executed")

tdCases.addLinux(__file__, TDTestCase())
tdCases.addWindows(__file__, TDTestCase())