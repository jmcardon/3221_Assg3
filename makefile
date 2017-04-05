#commands: make, make clean
HEADERS = errors.h
OBJECTS = New_Alarm_Cond.o

default: New_Alarm_Cond

#$< refers to the original object, $@ the target object
%.o: %.c $(HEADERS)
	cc -c $< -o $@ -lrt -lpthread

My_Alarm: $(OBJECTS)
	cc $(OBJECTS) -o $@ -lrt -lpthread

#test:
#	./My_Alarm >> Test_output.txt 2>> Test_output.txt


clean: 
	-rm -f $(OBJECTS)
	-rm -f New_Alarm_Cond
