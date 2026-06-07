if ( $?PATH ) then
  if ( ":$PATH:" !~ *":/usr/local/bin:"* ) then
    setenv PATH "/usr/local/bin:/usr/local/sbin:$PATH"
  endif
else
  setenv PATH "/usr/local/bin:/usr/local/sbin:/bin:/sbin:/usr/bin:/usr/sbin"
endif

if ( ! $?DISPLAY && ! $?SSH_TTY && -x /usr/local/bin/startx && ! -f /var/db/os-master/firstboot-setup-complete ) then
  if ( ! -f /tmp/os-master-autostartx.lock ) then
    /usr/bin/touch /tmp/os-master-autostartx.lock
    exec /usr/local/bin/startx
  endif
endif
