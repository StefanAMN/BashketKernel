# A small tour of the system. Run it with:  sh /etc/demo.sh
echo == who is running ==
ps
echo == memory ==
cat /proc/meminfo
echo == programs in /bin ==
ls /bin | wc
echo == a pipeline with redirection ==
echo one two three | wc > /tmp/demo.out
cat /tmp/demo.out
rm /tmp/demo.out
echo == environment ==
export DEMO=works
echo DEMO $DEMO, status of last command $?
echo == a program that misbehaves is stopped ==
fault kread
echo the shell is still here, status was $?
