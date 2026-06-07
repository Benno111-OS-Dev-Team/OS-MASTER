if ( $?PATH ) then
  if ( ":$PATH:" !~ *":/usr/local/bin:"* ) then
    setenv PATH "/usr/local/bin:/usr/local/sbin:$PATH"
  endif
else
  setenv PATH "/usr/local/bin:/usr/local/sbin:/bin:/sbin:/usr/bin:/usr/sbin"
endif
