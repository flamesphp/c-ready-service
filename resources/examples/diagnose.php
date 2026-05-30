<?php
/**
 * diagnose.php
 * Execute via browser ou CLI para identificar em qual modo o Flames Ready está.
 */

$mode = 'desconhecido';

if (xflames_ready_is_worker()) {
    $mode = 'WORKER (processo CLI forked pelo supervisor)';
} elseif (xflames_ready_is_supervisor()) {
    $mode = 'SUPERVISOR (processo CLI principal)';
} else {
    $sapi = php_sapi_name();
    $mode = "NÃO-WORKER ($sapi) – extension ativa, mas sem CLI supervisor";
}

$supervisorPid = xflames_ready_get_supervisor_pid();
$workerPids    = xflames_ready_get_worker_pids();

echo "=== Flames Ready Diagnóstico ===\n\n";
echo "Modo atual       : $mode\n";
echo "PID do processo  : " . getpid() . "\n";
echo "SAPI             : " . php_sapi_name() . "\n";
echo "is_ready()       : " . (xflames_ready_is_ready() ? 'true' : 'false') . "\n";
echo "request_count()  : " . xflames_ready_get_request_count() . "\n\n";

echo "=== Named Shared Memory ===\n";
if ($supervisorPid > 0) {
    echo "Supervisor PID   : $supervisorPid\n";
    echo "Workers vivos    : " . count($workerPids) . "\n";
    foreach ($workerPids as $i => $pid) {
        echo "  Worker #" . ($i + 1) . "  pid=$pid\n";
    }
} else {
    echo "Nenhum supervisor em execução (nenhuma named shm encontrada).\n";
    echo "→ Se você está em modo PHP-FPM, isso é esperado.\n";
    echo "→ Se você quer modo worker, rode: php worker.php\n";
}
