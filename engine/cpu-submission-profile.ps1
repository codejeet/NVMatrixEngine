# Bounded CPU wall-clock timeline. This validator adds no profiling process or
# GPU work during a capture; profile-cuda calls it only after the game exits.
function Get-CpuSubmissionProfile($report, $phases) {
  $trace=$report.latency.cpuSubmission
  $columns=@('frameIndex','renderBegin','fluidBegin','cudaPrepareBegin','cudaPrepared','interopBegin','interopContextReady',
    'prefixClosed','prefixSubmitted','dxReleased','cudaWaitQueued','cudaStartQueued','cudaWorkQueued','cudaEndQueued',
    'cudaReleaseQueued','dxWaitQueued','interopResumed','fluidRecorded','surfaceRecorded','photonsRecorded',
    'cameraRecorded','compositeRecorded','rrBegin','rrRecorded','uiRecorded','frameClosed','frameSubmitted',
    'presented','frameFenceSignaled','frameEventQueued','frameComplete','collected')
  if(!$trace -or $trace.version -ne 1 -or $trace.clock -cne 'steady_clock' -or
     $trace.units -cne 'milliseconds since renderBegin; CPU wall time, not GPU time' -or
     ($trace.columns -join '|') -cne ($columns -join '|') -or $trace.rows.Count -ne $report.latency.rows.Count){
    throw 'Missing/invalid CPU submission schema'
  }
  if($report.fluid.backend -notin @('cuda','dx12')){throw 'Unknown CPU trace backend'}
  $cuda=$report.fluid.backend -eq 'cuda'
  for($i=0;$i -lt $trace.rows.Count;++$i){
    $row=$trace.rows[$i]
    if($row.Count -ne $columns.Count -or $row[0] -ne $report.latency.rows[$i][0] -or
       $null -eq $row[1] -or $row[1] -ne 0){throw 'CPU trace frame/origin mismatch'}
    $previous=0.0
    for($j=1;$j -lt $row.Count;++$j){
      $value=$row[$j]
      if(!$cuda -and $j -ge 3 -and $j -le 16){
        if($null -ne $value){throw 'DX12 trace invented CUDA work'}
        continue
      }
      if($j -eq 29 -and $null -eq $value){continue}
      if(!($value -is [double] -or $value -is [decimal] -or $value -is [int] -or $value -is [long]) -or
         [double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt $previous){
        throw "Missing/nonmonotonic CPU stage $($columns[$j]) at frame $i"
      }
      $previous=$value
    }
    $whole=$report.latency.rows[$i][1];$renderer=$report.latency.rows[$i][7]
    $tolerance=.002+$whole*.00001
    if($previous -gt $whole+$tolerance -or $previous -lt $renderer-$tolerance){throw 'CPU trace does not cover renderer interval'}
  }
  $ranges=[ordered]@{renderSetup=@(1,2);fluidRecordTotal=@(2,17);surfaceRecord=@(17,18);
    photonsRecord=@(18,19);cameraRecord=@(19,20);compositeRecord=@(20,21);rrRecord=@(22,23);
    uiRecord=@(23,24);frameClose=@(24,25);frameSubmit=@(25,26);present=@(26,27);
    frameSignal=@(27,28);frameWait=@(28,30);collection=@(30,31)}
  if($cuda){
    $extra=[ordered]@{fluidSetup=@(2,3);cudaPreparation=@(3,4);interopContext=@(5,6);prefixClose=@(6,7);
      prefixSubmit=@(7,8);dxRelease=@(8,9);cudaWait=@(9,10);cudaStart=@(10,11);cudaEnqueue=@(11,12);
      cudaEnd=@(12,13);cudaRelease=@(13,14);dxWait=@(14,15);interopResume=@(15,16);
      postCudaRecord=@(16,25);prefixToSuffixSubmission=@(8,26)}
    foreach($key in $extra.Keys){$ranges[$key]=$extra[$key]}
  }
  $stats=[ordered]@{}
  foreach($phase in $phases.Keys){
    $interval=$phases[$phase];$result=[ordered]@{}
    foreach($name in $ranges.Keys){
      $bounds=$ranges[$name]
      $values=@(foreach($i in $interval[0]..$interval[1]){
        $trace.rows[$i][$bounds[1]]-$trace.rows[$i][$bounds[0]]
      })
      $result[$name]=Summary $values
    }
    $stats[$phase]=$result
  }
  $worst=@(foreach($frame in ($report.latency.rows[32..299]|Sort-Object {$_[1]} -Descending|Select-Object -First 10)){
    $row=$trace.rows[$frame[0]];$times=[ordered]@{}
    foreach($name in $ranges.Keys){$bounds=$ranges[$name];$times[$name]=$row[$bounds[1]]-$row[$bounds[0]]}
    @{frame=$frame[0];wholeFrame=$frame[1];cudaWork=$frame[13];cudaHandoff=$frame[14];cpu=$times}
  })
  @{phases=$stats;worstFrames=$worst}
}
