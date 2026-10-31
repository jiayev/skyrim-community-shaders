NR==1{ for(i=1;i<=NF;i++){ if($i=="MsBetweenDisplayChange") d=i; if($i=="TimeInMs") t=i } next }
{ v=$d+0; if(v<=0) next; n++; a[n]=v; tt[n]=$t+0; if(n==1) t0=tt[1] }
END{
  m=0; for(i=1;i<=n;i++) m+=a[i]; m/=n
  # 4 equal time segments
  span=(tt[n]-t0)/4
  for(i=1;i<=n;i++){ g=int((tt[i]-t0)/span); if(g>3)g=3
    c[g]++; s[g]+=a[i]; ss[g]+=a[i]*a[i] }
  for(g=0;g<4;g++){ mg=s[g]/c[g]; printf "  seg%d n=%3d mean=%.2f sd=%.3f\n", g, c[g], mg, sqrt(ss[g]/c[g]-mg*mg) }
}
