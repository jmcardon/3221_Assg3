#!/usr/bin/python2.7
import time
import pexpect
import sys
import os
import random
# Responses to be parsed
expect_arr = ["Display thread 1: Received", "Display thread 2: Received"]
# Spawn My_Alarm as a child process
child = pexpect.spawn("./New_Alarm_Cond")
#Set error tolerance
offset_tolerance = 0.015
errors = 0

if not os.path.isfile("program_log.txt"):
    os.system("touch program_log.txt");

program_log = open("program_log.txt", "w")

child.logfile_read = program_log
with open("testfile.txt", "r") as f:
    lines = f.readlines()
    responses = ["First Alarm Request With Message Number",
                 "Replacement Alarm Request With Message Number",
                 "Cancel Alarm Request With Message Number",
                 "Error: More Than One Request to Cancel Alarm Request With Message Number",
                 "Error: No Alarm Request With Message Number",
                 "Error: Incorrect format",
                 "Bad command"]
    for l in lines:
        try:
            # Send input to process
            child.sendline(l)
            # Parse which thread received the request

            index = child.expect(responses, timeout=30)
            # Measure the system time immediately after. Is subject to some error, as it's not at the same time stdout was printed
            # Also, from the time of receiving the alarm to printing there is a small delay,
            # as there are also delays due to locking of stdout.
            time_set = time.time()
            # Calculate the time offset to figure which thread the alarm should have gone.
            offset = time_set - int(time_set)
            if offset >= 0.5:
                time_offset = int(time_set) +1
            else:
                time_offset = int(time_set)

            # Output results
            print "Line sent: " + l.replace("\n","")
            print "Expected Response: " + responses[index]

            time.sleep(random.random())
        except KeyboardInterrupt:
            break

program_log.close()



