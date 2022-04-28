
from util.log import *
from util.sql import *
from util.cases import *

import os


class TDTestCase:

    def init(self, conn, logSql):
        tdLog.debug(f"start to excute {__file__}")
        tdSql.init(conn.cursor())

    def run(self):  # sourcery skip: extract-duplicate-method
        tdSql.prepare()
        # get system timezone
        time_zone = os.popen('timedatectl | grep zone').read(
        ).strip().split(':')[1].lstrip()

        tdLog.printNoPrefix("==========step1:create tables==========")
        tdSql.execute(
            '''create table if not exists ntb
            (ts timestamp, c1 int, c2 float,c3 double)
            '''
        )
        tdSql.execute(
            '''create table if not exists stb
            (ts timestamp, c1 int, c2 float,c3 double) tags(t0 int)
            '''
        )
        tdSql.execute(
            '''create table if not exists stb_1 using stb tags(100)
            '''
        )

        tdLog.printNoPrefix("==========step2:insert data==========")
        tdSql.execute(
            "insert into ntb values(now,10,99.99,11.111111)(today(),100,11.111,22.222222)")
        tdSql.execute(
            "insert into stb_1 values(now,111,99.99,11.111111)(today(),1,11.111,22.222222)")

        tdLog.printNoPrefix("==========step3:query data==========")
        
        tdSql.query("select timezone() from ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)
        tdSql.query("select timezone() from db.ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)
        tdSql.query("select timezone() from stb")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)
        tdSql.query("select timezone() from db.stb")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)
        tdSql.query("select timezone() from stb_1")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)
        tdSql.query("select timezone() from db.stb_1 ")
        tdSql.checkRows(2)
        tdSql.checkData(0, 0, time_zone)

        tdSql.error("select timezone(1) from stb")
        tdSql.error("select timezone(1) from db.stb")
        tdSql.error("select timezone(1) from ntb")
        tdSql.error("select timezone(1) from db.ntb")
        tdSql.error("select timezone(1) from stb_1")
        tdSql.error("select timezone(1) from db.stb_1")
        tdSql.error("select timezone(now()) from stb")
        tdSql.error("select timezone(now()) from db.stb")

        tdSql.query(f"select * from ntb where timezone()='{time_zone}'")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from db.ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from stb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from db.stb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from stb_1")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1 from db.stb_1")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1.5 from ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()+1.5 from db.ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()-100 from ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()*100 from ntb")
        tdSql.checkRows(2)
        tdSql.query("select timezone()/10 from ntb")
        # tdSql.query("select timezone()/0 from ntb")


        tdSql.query("select timezone()+null from ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()-null from ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()*null from ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()/null from ntb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        # tdSql.query("select timezone()")
        tdSql.query("select timezone()+null from stb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()-null from stb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()*null from stb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()/null from stb")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()+null from stb_1")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()-null from stb_1")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()*null from stb_1")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
        tdSql.query("select timezone()/null from stb_1")
        tdSql.checkRows(2)
        tdSql.checkData(0,0,None)
    def stop(self):
        tdSql.close()
        tdLog.success(f"{__file__} successfully executed")


tdCases.addLinux(__file__, TDTestCase())
tdCases.addWindows(__file__, TDTestCase())