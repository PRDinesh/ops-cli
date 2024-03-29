#!/usr/bin/python

# (c) Copyright 2015 Hewlett Packard Enterprise Development LP
#
# GNU Zebra is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
#
# GNU Zebra is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Zebra; see the file COPYING.  If not, write to the Free
# Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
# 02111-1307, USA.

from time import sleep
from opsvsi.docker import *
from opsvsi.opsvsitest import *


class FanSystemTests(OpsVsiTest):

    uuid = ''

    def setupNet(self):

        # if you override this function, make sure to
        # either pass getNodeOpts() into hopts/sopts of the topology that
        # you build or into addHost/addSwitch calls

        host_opts = self.getHostOpts()
        switch_opts = self.getSwitchOpts()
        fan_topo = SingleSwitchTopo(k=0, hopts=host_opts, sopts=switch_opts)
        self.net = Mininet(fan_topo, switch=VsiOpenSwitch,
                           host=Host, link=OpsVsiLink,
                           controller=None, build=True)

    def initFanTable(self):

        # Add dummy data for fans in subsystem and fan table for simulation.
        # Assume there would be only one entry in subsystem table

        s1 = self.net.switches[0]
        print '\n'
        out = s1.ovscmd('ovs-vsctl list subsystem')
        lines = out.split('\n')
        for line in lines:
            if '_uuid' in line:
                _id = line.split(':')
                FanSystemTests.uuid = _id[1].strip()
                out = s1.ovscmd('/usr/bin/ovs-vsctl -- set Subsystem '
                                + FanSystemTests.uuid
                                + ' fans=@fan1 -- --id=@fan1 create Fan '
                                + ' name=base-FAN-1L direction=f2b '
                                + ' speed=normal '
                                + ' status=ok rpm=9000'
                                )

    def deinitFanTable(self):
        s1 = self.net.switches[0]

        # Delete dummy data from subsystem and led table to avoid clash
        # with other CT scripts.

        s1.ovscmd('ovs-vsctl clear subsystem ' + FanSystemTests.uuid
                  + ' fans')

    def showSystemFanTest(self):

        # Test to verify show system command

        s1 = self.net.switches[0]
        counter = 0
        info('''
########## Test to verify \'show system fan\' command ##########
''')
        fan_keywords_found = False
        out = s1.cmdCLI('show system fan')
        lines = out.split('\n')
        for line in lines:
            if 'base-FAN-1L' in line:
                counter += 1
            if 'front-to-back' in line:
                counter += 1
            if 'normal' in line:
                counter += 1
            if 'ok' in line:
                counter += 1
            if '9000' in line:
                counter += 1
        if counter == 5:
            fan_keywords_found = True
        assert fan_keywords_found is True, \
            ' Test to verify \'show system fan\' command - FAILED!'
        return True

    def setSystemFanSpeedTest(self):

        # Test to verify fan-speed command

        s1 = self.net.switches[0]
        info('''
########## Test to verify \'fan-speed\' command  ##########
''')
        fan_speed_set = False
        out = s1.cmdCLI('configure terminal')
        out = s1.cmdCLI('fan-speed slow')
        out = s1.cmdCLI('do show system fan')
        s1.cmdCLI('exit')
        lines = out.split('\n')
        for line in lines:
            if 'Fan speed override is set to : slow' in line:
                fan_speed_set = True
        assert fan_speed_set is True, \
            'Test to verify \'fan-speed\' command - FAILED!'
        return True

    def showrunningFanSpeed(self):

        # Test to verify if the fan-speed config is reflected
        # in show running config

        s1 = self.net.switches[0]
        info("########## Test to verify \'show running\' command for"
             " fan-speed config ##########\n")
        fan_speed_keyword_found = False
        out = s1.cmdCLI('show running-config')
        lines = out.split('\n')
        for line in lines:
            if 'fan-speed slow' in line:
                fan_speed_keyword_found = True
        assert fan_speed_keyword_found is True, \
            'Test to verify \'show running\' command'\
            'for fan-speed config - FAILED!'
        return True

    def unsetSystemFanSpeedTest(self):

        # Test to verify no fan-speed command

        s1 = self.net.switches[0]
        info('''
########## Test to verify \'no fan-speed\' command ##########
''')
        fan_speed_unset = False
        out = s1.cmdCLI('configure terminal')
        if 'Unknown command' in out:
            print out
            return False
        out = s1.cmdCLI('no fan-speed')
        if 'Unknown command' in out:
            print out
            return False
        out = s1.cmdCLI('do show system fan')
        s1.cmdCLI('exit')
        lines = out.split('\n')
        for line in lines:
            if 'Fan speed override is not configured' in line:
                fan_speed_unset = True
        assert fan_speed_unset is True, \
            'Test to verify \'no fan-speed\' command - FAILED!'
        return True


class Test_sys_fan:

    def setup(self):
        pass

    def teardown(self):
        pass

    def setup_class(cls):

        # Initialize the led table with dummy value

        Test_sys_fan.test = FanSystemTests()
        Test_sys_fan.test.initFanTable()

    # show system fan test.

    def test_show_system_fan_command(self):
        if self.test.showSystemFanTest():
            info('''
########## Test to verify \'show system fan\' command - SUCCESS! ##########
''')

    # set system fan speed test.

    def test_set_system_fan_speed_command(self):
        if self.test.setSystemFanSpeedTest():
            info('''
########## Test to verify \'fan-speed\' command - SUCCESS! ##########
''')

    # showrunningFanSpeed

    def test_show_run_for_fan_speed_config(self):
        if self.test.showrunningFanSpeed():
            info('''
########## Test to verify \'show running\' command '''
                 '''for fan-speed config - SUCCESSS! ##########
''')

    # unset system fan speed test

    def test_unset_system_fan_speed_command(self):
        if self.test.unsetSystemFanSpeedTest():
            info('''
########## Test to verify \'no fan-speed\' command - SUCCESS! ##########
''')

    def teardown_class(cls):

        # Delete Dummy data to avoid clash with other test scripts

        Test_sys_fan.test.deinitFanTable()

        # Stop the Docker containers, and
        # mininet topology

        Test_sys_fan.test.net.stop()

    def setup_method(self, method):
        pass

    def teardown_method(self, method):
        pass

    def __del__(self):
        del self.test
