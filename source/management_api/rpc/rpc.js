// MIT License
//
// Copyright (c) 2012 Universidad Politécnica de Madrid
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Copyright (C) <2019> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

// This file is borrowed from lynckia/licode with some modifications.

'use strict';
var amqp = require('amqp');
var log = require('./../logger').logger.getLogger('RPC');
var TIMEOUT = 3000;
var corrID = 0;
var map = {};   //{corrID: {fn: callback, to: timeout}}
var clientQueue;
var connection;
var exc;

exports.connect = function (addr) {
    connection = amqp.createConnection(addr);
    connection.on('ready', function () {
        log.info('Connected to rabbitMQ server');

        //Create a direct exchange
        exc = connection.exchange('woogeenRpc', {type: 'direct'}, function (exchange) {
            log.info('Exchange ' + exchange.name + ' is open');

            //Create the queue for send messages
            clientQueue = connection.queue('', function (q) {
                log.info('ClientQueue ' + q.name + ' is open');
                clientQueue.bind('woogeenRpc', clientQueue.name);
                clientQueue.subscribe(function (message) {
                    if (map[message.corrID] !== undefined) {
                        map[message.corrID].fn[message.type](message.data, message.err);
                        clearTimeout(map[message.corrID].to);
                        delete map[message.corrID];
                    }
                });
            });
        });
    });
};

exports.disconnect = function() {
    if (connection) {
        connection.disconnect();
        connection = undefined;
    }
};

var callbackError = function (corrID) {
    for (var i in map[corrID].fn) {
        map[corrID].fn[i]('timeout');
    }
    delete map[corrID];
};

/*
 * Calls remotely the 'method' function defined in rpcPublic of 'to'.
 */
exports.callRpc = function (to, method, args, callbacks, timeout) {
    if (!clientQueue) {
        for (var i in callbacks) {
            callbacks[i]('rpc client not ready');
        }
        return;
    }
    corrID += 1;
    map[corrID] = {};
    map[corrID].fn = callbacks;
    map[corrID].to = setTimeout(callbackError, ((typeof timeout === 'number' && timeout) ? timeout : TIMEOUT), corrID);
    var send = {method: method, args: args, corrID: corrID, replyTo: clientQueue.name};
    exc.publish(to, send);
};
