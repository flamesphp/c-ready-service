<?php

use Flames\Ready\Ready\Service;
use Flames\Ready\Ready\Service\Worker;
use Flames\Ready\Ready\Service\Supervisor;
use Flames\Ready\Ready\Service\Register;

if (!Service::isReady()) {
    Register::load('App', 'boot');
    Register::reset('App', 'reset');
}

header('Content-Type: application/json');
echo json_encode([
    'mode'          => Worker::isWorker() ? 'worker' : 'fpm',
    'pid'           => getpid(),
    'is_ready'      => Service::isReady(),
    'req_count'     => Worker::getRequestCount(),
    'supervisor'    => Supervisor::getPid(),
]);
