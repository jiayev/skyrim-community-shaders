NR==1{ for(i=1;i<=NF;i++){ if($i=="MsBetweenDisplayChange") d=i; if($i=="MsBetweenPresents") p=i } next }
{ v=$d+0; if(v<=0) next; n++; s+=v; ss+=v*v; a[n]=v }
END{
  m=s/n; sd=sqrt(ss/n-m*m)
  printf "n=%d mean=%.3f sd=%.3f\n", n, m, sd
  # deviation from mean, bucketed
  for(i=1;i<=n;i++){ e=a[i]-m; if(e<0)e=-e
    if(e<0.5)b1++; else if(e<1)b2++; else if(e<2)b3++; else if(e<4)b4++; else if(e<10)b5++; else b6++ }
  printf "|dev| <0.5ms %5.1f%%  0.5-1 %4.1f%%  1-2 %4.1f%%  2-4 %4.1f%%  4-10 %4.1f%%  >10 %4.1f%%\n",
    100*b1/n,100*b2/n,100*b3/n,100*b4/n,100*b5/n,100*b6/n
  # sd with the worst 1% of |dev| removed
  for(i=1;i<=n;i++){ e=a[i]-m; d2[i]=(e<0?-e:e) }
  k=int(n*0.01); if(k<1)k=1
  # simple selection of threshold
  asort_n=n
  for(i=1;i<=n;i++) srt[i]=d2[i]
  # insertion-free: find k-th largest by counting
  lo=0; hi=100
  for(it=0;it<40;it++){ mid=(lo+hi)/2; c=0; for(i=1;i<=n;i++) if(d2[i]>mid)c++; if(c>k) lo=mid; else hi=mid }
  thr=hi
  s2=0;ss2=0;n2=0
  for(i=1;i<=n;i++) if(d2[i]<=thr){ n2++; s2+=a[i]; ss2+=a[i]*a[i] }
  m2=s2/n2; sd2=sqrt(ss2/n2-m2*m2)
  printf "excluding worst 1%% (|dev|>%.2fms): n=%d sd=%.3f\n", thr, n2, sd2
}
