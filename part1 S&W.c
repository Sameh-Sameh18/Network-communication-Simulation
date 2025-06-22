/*
the code works briefly that for each delay we fire a timer to delay the forwarding process with required time
and the sequence is made per destination as if each sender send to a specific receiver it increments if the packet 
is sent to same receiver

in final stats, you will find that there is dropped number for each receiver from each sender it indicates the 
packets dropped at each time it sent to the receiver while "Packets Dropped After Max Retries" indicates the packets 
dropped after the retransmiting also for low P_drop it gives zero as it will be retransmitted again so the posibility is decreased more 
while when increasing P_drop or number of packets significantly it gives a value
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

// some constants to make modification and the tests easier
#define L1                      500 // min length
#define L2                      1500 // max length
#define HEADER_SIZE             7 // size of header "H"
#define MAX_IN_QUEUE            5 // max queue size
#define T1                      100 // min time
#define T2                      200 // max time
#define D				        5 // propagation delay constant
#define C                       100000 // link capacity in bits/sec
#define ACK_SIZE                40 // acknowledge size 
#define P_drop                  1 // P_drop in %
#define ACK_drop                1 // ACK drop
#define Tout                    225 // the Tout in msec
#define MAX_RETRIES             4 // max number of retransmissions
#define TARGET_PACKETS_PER_RECEIVER 2000 // number of target received packets 

// packet struct
typedef struct {
    uint8_t  senderID; // sender of the packet (sender1 or dender2)
    uint8_t  dest; // receiver of the packer (receiver3 or receiver4)
    uint32_t seq; // seq of the packet
    uint16_t len; // packet length
    uint8_t *payload; // pointer to data of packet
} Packet;

// ack struct
typedef struct {
    uint8_t senderID;     // sender of the original packet (destination for ACK)
    uint8_t receiverID;   // receiver sending the ACK (source for ACK)
    uint32_t seq;
} Ack;

// queues
QueueHandle_t senderQueues[2];
QueueHandle_t receiverQueues[2];
QueueHandle_t ackQueues[2]; // one for each sender

// Mutex-protected counters 
SemaphoreHandle_t counterMutex;
SemaphoreHandle_t forwardingMutex;

// sender state struct
typedef struct {
    Packet *pendingPacket;
    uint32_t pendingSeq;
    uint8_t pendingDest;
    uint8_t retries;
    TimerHandle_t timer;
    BaseType_t acked;
    uint32_t totalTransmissions;
    uint32_t droppedAfterMaxRetries;
    uint32_t totalRetries;
} SWState;

SWState senderStates[2] = {0}; // each sender state intialized by zero (0->sender1 & 1->sender2)

uint32_t recv_count[2] = {0, 0};  // tracks packets received by receiver3 and receiver4
uint32_t seqPerDest[2] = {0, 0};  // sequence numbers per destination
uint32_t dropCountBySender[2][2] = {{0}};  // drop for each sender from each sender and index is as [receiver][sender]
uint32_t dropCountAck[2] = {0, 0};         // drop of ack sent to each sender
volatile int activeForwardingCounters = 0;  // pending forward operations counter 

uint32_t totalBytesDelivered[2] = {0, 0}; // counter for total bytes sent to the each receiver
TickType_t T_start = 0; // start time of the task
TickType_t T_end = 0; // end time of the task
// timers used to calc the simulation duration to be used in throughput calcs

// timer intializing
TimerHandle_t switchTimer;

// Hooks of FreeRTOS
void vApplicationMallocFailedHook(void) { printf("Malloc failed!\n"); for(;;); }
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) { printf("Stack overflow: %s\n", pcTaskName); for(;;); }
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}

// generate random values between 2 values function
int randomInRange(int min, int max) {
    return min + rand() % (max - min + 1);
}

// generate random value for the destination function 
uint8_t pickDest(void) {
    return (rand() % 2) + 3;
}

// transmission delay function
uint32_t transmissionDelayBytes(uint32_t bytes) {
    return ((bytes * 8 * 1000) / C);
}

// Forwarding for Data and ACKs
void forwardDataCallback(TimerHandle_t xTimer);
void forwardAckCallback(TimerHandle_t xTimer);

// Sender Timeout Callback
void senderTimeoutCallback(TimerHandle_t xTimer) {
    int idx = (int)(intptr_t)pvTimerGetTimerID(xTimer); // get sender index from timer ID
    SWState *state = &senderStates[idx]; // get sender state

    if (!state->pendingPacket) return; // return if no pending packet

    if (state->acked) return; // check the ACK

    // retransmit for unacked packets and change the counter of retries
    if (state->retries < MAX_RETRIES) { 
        state->retries++;
        state->totalRetries++;
        state->totalTransmissions++;
        uint32_t txDelay = transmissionDelayBytes(state->pendingPacket->len) + D; // calculate overall delay
        Packet *copy = (Packet *)pvPortMalloc(sizeof(Packet)); 
        memcpy(copy, state->pendingPacket, sizeof(Packet));
        copy->payload = pvPortMalloc(state->pendingPacket->len - HEADER_SIZE);
        memcpy(copy->payload, state->pendingPacket->payload, state->pendingPacket->len - HEADER_SIZE);
        // create forwarding timer to delay the sending by the required delay
        TimerHandle_t fwdTimer = xTimerCreate(
            "FWD", pdMS_TO_TICKS(txDelay), pdFALSE, copy, forwardDataCallback
        );
        xTimerStart(fwdTimer, 0);
        // show the process is done
        printf("Sender%u: Retransmitting seq=%lu (try %u)\n",
               idx + 1, (unsigned long)state->pendingSeq, state->retries );

        // restart timeout timer
        xTimerChangePeriod(state->timer, pdMS_TO_TICKS(Tout), 0);
    } else {
        // max retries reached so drop the packet and send another
        state->droppedAfterMaxRetries++; // increment the counter of the dropped after max retries
        // show that the packet is dropped
        printf("Sender%u: Dropping seq=%lu after %u tries\n",
               idx + 1, (unsigned long)state->pendingSeq, MAX_RETRIES );
        vPortFree(state->pendingPacket->payload);
        vPortFree(state->pendingPacket);
        state->pendingPacket = NULL;
        state->retries = 0;
        state->acked = pdFALSE;
    }
}

void senderTask(void *pv) {
    int idx = (int)(intptr_t)pv; // get sender index from parameter
    SWState *state = &senderStates[idx]; // get sender state

    while (1) {
        // check if there is a packet waiting theack on the TX buffer or not
        // if there isn't start to generate the packet and send it
        if (!state->pendingPacket) {
            // generate a new packet
            Packet *p = (Packet *)pvPortMalloc(sizeof(Packet));
            p->senderID = idx + 1;
            p->dest = pickDest();
            p->len = randomInRange(L1, L2);
            p->payload = pvPortMalloc(p->len - HEADER_SIZE);

            xSemaphoreTake(counterMutex, portMAX_DELAY);
            p->seq = seqPerDest[p->dest - 3]++; // increament the sequence for each receiver
            xSemaphoreGive(counterMutex);

            memset(p->payload, 0, p->len - HEADER_SIZE);
            // set the sender state 
            state->pendingPacket = p;
            state->pendingSeq = p->seq;
            state->pendingDest = p->dest;
            state->retries = 0;
            state->acked = pdFALSE;
            state->totalTransmissions++;

            // send the packet via switch
            uint32_t txDelay = transmissionDelayBytes(p->len) + D; // calculate overall delay
            Packet *copy = (Packet *)pvPortMalloc(sizeof(Packet));
            memcpy(copy, p, sizeof(Packet));
            copy->payload = pvPortMalloc(p->len - HEADER_SIZE);
            memcpy(copy->payload, p->payload, p->len - HEADER_SIZE);
            // create forward time to forward the packet to the receiver
            TimerHandle_t fwdTimer = xTimerCreate(
                "FWD", pdMS_TO_TICKS(txDelay), pdFALSE, copy, forwardDataCallback
            );
            xTimerStart(fwdTimer, 0);
            // show the process state 
            printf("Sender%u → dest=%u seq=%lu len=%u\n",
                   p->senderID, p->dest, (unsigned long)p->seq, p->len);

            // start timeout timer to retransmit
            if (state->timer)
                xTimerDelete(state->timer, 0);
            state->timer = xTimerCreate(
                "Tout", pdMS_TO_TICKS(Tout), pdFALSE, (void *)(intptr_t)idx, senderTimeoutCallback
            );
            xTimerStart(state->timer, 0);
        }

        // random interval between packets generation
        uint32_t interval = T1 + rand() % (T2 - T1 + 1);
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

// ACK handler for each sender
void vAckHandler(void *pv) {
    int idx = (int)(intptr_t)pv; // get sender index from parameter
    SWState *state = &senderStates[idx]; // get sender state
    Ack ack; // declare ACK structure
    while (1) {
        if (xQueueReceive(ackQueues[idx], &ack, portMAX_DELAY) == pdTRUE) {
            // check the matching of the ack of destination and source also if it is acked or not
            // checks whether the received ACK corresponds to the sender’s current pending packet 
            // as if all conditions are met, the ACK is valid
            if (state->pendingPacket &&
                ack.seq == state->pendingSeq &&
                ack.receiverID == state->pendingDest &&
                !state->acked) {
                // stop timeout timer
                if (state->timer)
                    xTimerStop(state->timer, 0);
                vPortFree(state->pendingPacket->payload);
                vPortFree(state->pendingPacket);
                state->pendingPacket = NULL;
                printf("Sender%u: ACK received for seq=%lu\n",
                       idx + 1, (unsigned long)ack.seq);
                state->retries = 0;  // reset retries
                state->acked = pdTRUE; // set acknowledged flag
            }
        }
    }
}

// Forwarding data packet via switch 
void forwardDataCallback(TimerHandle_t xTimer) {
    Packet *pkt = (Packet *)pvTimerGetTimerID(xTimer);
    int destIdx = pkt->dest - 3; // as index: 0->receiver3 & 1->receiver4
    int senderIdx = pkt->senderID - 1; // as index: 0->sender1 & 1->sender2

    // simulate drop of the packet
    if ((rand() % 100) < P_drop) {
        xSemaphoreTake(counterMutex, portMAX_DELAY);
        dropCountBySender[destIdx][senderIdx]++; // increament the drop counter "dropped from sender not lost"
        xSemaphoreGive(counterMutex);
        // show the process state
        printf("Switch: Dropped from S%u → R%u seq=%lu\n",
               pkt->senderID, pkt->dest, (unsigned long)pkt->seq);
        vPortFree(pkt->payload);
        vPortFree(pkt);
    } else {
        // forward to receiver
        xQueueSend(receiverQueues[destIdx], &pkt, 0);
        printf("Switch: Delivered to R%u seq=%lu from S%u\n",
               pkt->dest, (unsigned long)pkt->seq, pkt->senderID);
    }
    xTimerDelete(xTimer, 0);
}

// Forwarding ACK via switch 
void forwardAckCallback(TimerHandle_t xTimer) {
    Ack *ack = (Ack *)pvTimerGetTimerID(xTimer);  // get ACK from timer ID
    int senderIdx = ack->senderID - 1; //// calculate sender index

    // simulate drop of the ack
    if ((rand() % 100) < ACK_drop) {
        xSemaphoreTake(counterMutex, portMAX_DELAY);
        dropCountAck[senderIdx]++;
        xSemaphoreGive(counterMutex);
        // show the process state
        printf("Switch: Dropped ACK to S%u for seq=%lu\n",
               ack->senderID, (unsigned long)ack->seq);
        vPortFree(ack);
    } else {
        // forward to sender's ACK queue
        xQueueSend(ackQueues[senderIdx], ack, 0);
        printf("Switch: Delivered ACK to S%u for seq=%lu\n",
               ack->senderID, (unsigned long)ack->seq);
        vPortFree(ack);
    }
    xTimerDelete(xTimer, 0);
}

// Switch timer callback processes incoming packets
void switchTimerCallback(TimerHandle_t xTimer) {
    Packet *pkt;
    for (int i = 0; i < 2; ++i) {
        if (xQueueReceive(senderQueues[i], &pkt, 0) == pdTRUE) {
            // forward with simulated delay 
            //free the memory 
            vPortFree(pkt->payload);
            vPortFree(pkt);
        }
    }
}

void vReceiver(void *pv) {
    int recIdx = (int)(intptr_t)pv; // get receiver index from parameter
    QueueHandle_t inQ = receiverQueues[recIdx]; // get receiver's input queue

    while (1) {
        Packet *pkt;
        if (xQueueReceive(inQ, &pkt, portMAX_DELAY) == pdTRUE) { // receive packet from queue
            // check the packet destination is correct ornot
            if (pkt->dest != (recIdx + 3)) {
                // show the process state
                printf("Receiver%u: ERROR - received packet for %u\n",
                       recIdx + 3, pkt->dest);
                vPortFree(pkt->payload);
                vPortFree(pkt);
                continue;
            }

            xSemaphoreTake(counterMutex, portMAX_DELAY);
            recv_count[recIdx]++; // increament the received packets counter
            totalBytesDelivered[recIdx] += pkt->len; // calculate the total bytes of received packets
            xSemaphoreGive(counterMutex);

            printf("Receiver%u: Received seq=%lu from S%u (TotalRecv=%lu)\n",
                   recIdx + 3, (unsigned long)pkt->seq, pkt->senderID, (unsigned long)recv_count[recIdx]);

            // send ACK to sender via switch
            Ack *ack = (Ack *)pvPortMalloc(sizeof(Ack));
            ack->senderID = pkt->senderID;
            ack->receiverID = pkt->dest;
            ack->seq = pkt->seq;



            uint32_t txDelay = transmissionDelayBytes(ACK_SIZE) + D;
            // create timer to delay the ack sending
            TimerHandle_t ackTimer = xTimerCreate( 
                "ACK", pdMS_TO_TICKS(txDelay), pdFALSE, ack, forwardAckCallback
            );
            xTimerStart(ackTimer, 0);

            vPortFree(pkt->payload);
            vPortFree(pkt);
        }
    }
}


void vMonitor(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        xSemaphoreTake(counterMutex, portMAX_DELAY); // lock counter mutex
        // check if it reaches the target packets
        int allReceived = (recv_count[0] >= TARGET_PACKETS_PER_RECEIVER &&
                           recv_count[1] >= TARGET_PACKETS_PER_RECEIVER);
        T_end=xTaskGetTickCount();

        xSemaphoreGive(counterMutex);

        if (allReceived) {
            // wait for all pending operations to complete and checks if there is packets stuck in the switch
            while (uxQueueMessagesWaiting(receiverQueues[0]) ||
                   uxQueueMessagesWaiting(receiverQueues[1]) ||
                   activeForwardingCounters != 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            // print final stats
            xSemaphoreTake(counterMutex, portMAX_DELAY);
            printf("\n=== FINAL STATS ===\n");
            uint32_t period = (uint32_t)((unsigned)T_end -(unsigned)T_start) / 1000;
            printf("Simulation time: start time = %u msec & end time = %u msec \n",
                          (unsigned)T_start,(unsigned)T_end);
            uint32_t totalBytes = totalBytesDelivered[0]+totalBytesDelivered[1];

            uint32_t throughput = (totalBytes / period) ;
            printf("Total bytes received by both receivers: %u \n",
                    	(unsigned)totalBytes);

            for (int r = 0; r < 2; r++) {
                printf("Receiver%u Received: %lu\n", r + 3, (unsigned long)recv_count[r]);
                printf("  Dropped from Sender1: %lu\n", (unsigned long)dropCountBySender[r][0]);
                printf("  Dropped from Sender2: %lu\n", (unsigned long)dropCountBySender[r][1]);
            }
            for (int s = 0; s < 2; s++) {
                printf("Sender%u:\n", s + 1);
                printf("  Total Transmissions: %lu\n", (unsigned long)senderStates[s].totalTransmissions);
                printf("  Packets Dropped After Max Retries: %lu\n", (unsigned long)senderStates[s].droppedAfterMaxRetries);
                printf("  Total Retransmissions: %lu\n", (unsigned long)senderStates[s].totalRetries);
                printf("  ACKs Dropped: %lu\n", (unsigned long)dropCountAck[s]);
            }
            printf("Throughput: %lu \n",(unsigned)throughput);
            printf("===================\n");
            xSemaphoreGive(counterMutex);

            vTaskEndScheduler();
            for(;;);
        }
    }
}

// Initialization of timer for eaiser debugging 
void vInitTask(void *pv) {
    switchTimer = xTimerCreate("Switch", pdMS_TO_TICKS(T2), pdTRUE, NULL, switchTimerCallback);
    xTimerStart(switchTimer, 0);
    vTaskDelete(NULL);
}


int main(void) {
    srand(12345); // seed for reproducibility
    T_start = xTaskGetTickCount();
    senderQueues[0] = xQueueCreate(MAX_IN_QUEUE, sizeof(Packet *));
    senderQueues[1] = xQueueCreate(MAX_IN_QUEUE, sizeof(Packet *));
    receiverQueues[0] = xQueueCreate(MAX_IN_QUEUE, sizeof(Packet *));
    receiverQueues[1] = xQueueCreate(MAX_IN_QUEUE, sizeof(Packet *));
    ackQueues[0] = xQueueCreate(MAX_IN_QUEUE, sizeof(Ack));
    ackQueues[1] = xQueueCreate(MAX_IN_QUEUE, sizeof(Ack));

    counterMutex = xSemaphoreCreateMutex();
    forwardingMutex = xSemaphoreCreateMutex();

    xTaskCreate(vInitTask, "Init", 1000, NULL, 1, NULL);
    xTaskCreate(senderTask, "Sender1", 2048, (void *)0, 2, NULL);
    xTaskCreate(senderTask, "Sender2", 2048, (void *)1, 2, NULL);
    xTaskCreate(vReceiver, "Receiver3", 1000, (void *)0, 1, NULL);
    xTaskCreate(vReceiver, "Receiver4", 1000, (void *)1, 1, NULL);
    xTaskCreate(vAckHandler, "AckHandler1", 1000, (void *)0, 1, NULL);
    xTaskCreate(vAckHandler, "AckHandler2", 1000, (void *)1, 1, NULL);
    xTaskCreate(vMonitor, "Monitor", 1000, NULL, 2, NULL);

    vTaskStartScheduler();
    for (;;);
    return 0;
}


