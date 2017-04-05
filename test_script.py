#!/usr/bin/python2.7
import time
import pexpect
import os
import random
# Spawn My_Alarm as a child process
child = pexpect.spawn("./New_Alarm_Cond")
#Set error tolerance

if not os.path.isfile("program_log.txt"):
    os.system("touch program_log.txt");

program_log = open("program_log.txt", "w")

child.logfile_read = program_log
with open("test_file.txt", "r") as f:
    lines = f.readlines()
    responses = ["First Alarm Request With Message Number",
                 "Replacement Alarm Request With Message Number",
                 "Cancel Alarm Request With Message Number",
                 "Error: Incorrect format",
                 "Bad command"]
    responses_cancel = ["Error: No Alarm Request With Message Number",
                        "Error: More Than One Request to Cancel Alarm Request With Message Number",
                        "Display thread exiting at time"]
    for l in lines:
        try:
            # Send input to process
            child.sendline(l)
            # Parse which thread received the request
            if "Cancel" in l:
                index = child.expect(responses_cancel)
                print "Line sent: " + l.replace("\n","")
                print "Expected Response: " + responses_cancel[index]
            else:
                index = child.expect(responses, timeout=30)
                # Output results
                print "Line sent: " + l.replace("\n","")
                print "Expected Response: " + responses[index]

            #Do not flood messages.
            # time.sleep(random.random())
        except KeyboardInterrupt:
            break
        except pexpect.EOF:
            print "LOL"
            continue

program_log.close()



