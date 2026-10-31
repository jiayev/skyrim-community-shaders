NR==1{for(i=1;i<=NF;i++)h[$i]=i; next}
{ bp=$(h["MsBetweenPresents"]); dc=$(h["MsBetweenDisplayChange"]); pm=$(h["PresentMode"]);
  n++; s+=bp; s2+=bp*bp; if(bp<1.0)near0++;
  if(dc!=""&&dc+0>0){d++; sd_+=dc; sd2+=dc*dc} else nodisp++;
  mode[pm]++ }
END{ m=s/n; sd=sqrt(s2/n-m*m);
  printf "  presents %d | interval mean %.2f ms sd %.2f ms (%.1f fps)\n", n, m, sd, 1000/m
  printf "  back-to-back (<1ms): %d (%.1f%%)   never displayed: %d (%.1f%%)\n", near0, 100*near0/n, nodisp, 100*nodisp/n
  if(d){md=sd_/d; printf "  displayed %d | mean %.2f ms (%.1f fps)\n", d, md, 1000/md}
  printf "  modes: "; for(x in mode) printf "%s=%d ", x, mode[x]; print "" }
