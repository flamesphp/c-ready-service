<?php

use Flames\Ready\Ready\Service\Worker;
use Flames\Ready\Ready\Service\Supervisor;

$supervisorPid = Supervisor::getPid();
$workerPids    = Worker::getPids();

if ($supervisorPid === null) {
    echo "Nenhum supervisor em execução.\n";
    exit(1);
}

echo "Supervisor PID : $supervisorPid\n";
echo "Workers vivos  : " . count($workerPids) . "\n";

foreach ($workerPids as $i => $pid) {
    $alive = posix_kill($pid, 0) ? 'alive' : 'dead';
    echo "  Worker #" . ($i + 1) . "  pid=$pid  [$alive]\n";
}

echo "\nEste processo:\n";
echo "  is_worker     : " . (Worker::isWorker()         ? 'true' : 'false') . "\n";
echo "  is_supervisor : " . (Supervisor::isSupervisor()  ? 'true' : 'false') . "\n";
echo "  is_ready      : " . (Service::isReady()          ? 'true' : 'false') . "\n";
