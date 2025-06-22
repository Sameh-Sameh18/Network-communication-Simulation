/*
for high TARGET_PACKETS_PER_RECEIVER you should increase the heap size

the logic of this part is very similar to part 1 with small modifications 
the most obvious one is that the sequence is unique per destination and source together
not one of them
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

// // some constants to make modification and the tests easier
#define N 8 // window size
#define T_out 175 //
#define L1 500 // min length of the packet
#define L2 1500 // max length of the packet
#define T1 100 // min time of delay
#define T2 200 // max time of delay
#define D 5 // propagation delay constant
#define C 100000 // link capacity in bits/sec
#define P_DROP 2 // P_drop in %
#define P_ACK 1 // ACK drop
#define ACK_LENGTH 40 // ACK length
#define TARGET_PACKETS_PER_RECEIVER  1000 // the number of receives required for each receiver 

// packet Structure
typedef struct {
    uint8_t sourceID;
    uint8_t dest;
    uint32_t seq;
    uint16_t len;
    uint8_t *pyload
} Packet;

// ACK Structure
typedef struct {
    uint8_t senderID;      // Receiver sending ACK
    uint8_t receiverID; // Original sender
    uint32_t seq;    // Next expected sequence number
} Ack;

// Window structure 
typedef struct {
    uint32_t base;
    uint32_t next_seq;
    Packet *window[N];
    int packet_retries[N];
    int start;
    int count;
    TimerHandle_t timer;
    uint8_t receiver_id;
} Window;

typedef struct {
    Window windows[2]; // 0 for receiver 3, 1 for receiver 4
} SenderState;

SenderState sender_states[2];

typedef struct {
    uint32_t expected_seq_s1;
    uint32_t expected_seq_s2;
} ReceiverSeqState;

// Queue 
QueueHandle_t S1_Switch;
QueueHandle_t S2_Switch;
QueueHandle_t Switch_R3;
QueueHandle_t Switch_R4;
QueueHandle_t R3_Switch_ACK;
QueueHandle_t R4_Switch_ACK;
QueueHandle_t Switch_S1_ACK;
QueueHandle_t Switch_S2_ACK;

// global counters
uint32_t receiverpcks[2]={0,0};
uint32_t droppedACK = 0;
uint32_t dropped_after_max_retries = 0;
uint32_t receivedBySender[2][2]  = {{0}};
uint32_t dropCountBySender[2][2] = {{0}}; // [receiver][sender]
TickType_t T_start;
TickType_t T_end;
uint32_t totalByteReceived[2] = {0};
volatile uint32_t total_received = 0;
volatile uint32_t total_dropped = 0;
uint32_t total_generated = 0;
uint32_t total_transmissions = 0;
uint32_t total_retransmissions = 0;

// semaphores
SemaphoreHandle_t received_mutex;
SemaphoreHandle_t dropped_mutex;
SemaphoreHandle_t generated_mutex;
SemaphoreHandle_t transmissions_mutex;

ReceiverSeqState receiver3_seq_state = {0, 0};
ReceiverSeqState receiver4_seq_state = {0, 0};

volatile BaseType_t stop_generating = pdFALSE;

// Hooks of FreeRTOS
void vApplicationMallocFailedHook(void) { printf("Malloc failed!\n"); for(;;); }
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) { printf("Stack overflow: %s\n", pcTaskName); for(;;); }
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}

// Callback Functions prototype 
void forward_data_callback(TimerHandle_t xTimer);
void forward_ack_callback(TimerHandle_t xTimer);
void sender_timeout_callback(TimerHandle_t xTimer);

// generate random values between 2 values function
int randomInRange(int min, int max) {
    return min + rand() % (max - min + 1);
}

// generate random value for the destination function
uint8_t pickDest(void) {
    return (rand() % 2) + 3;
}

// generate the transmision delay function 
uint32_t transmission_delay_ms(uint32_t bytes) {
    return (uint32_t)((bytes * 8 * 1000) / C);
}

void sender_timeout_callback(TimerHandle_t xTimer) {
    Window *win = (Window *)pvTimerGetTimerID(xTimer);
    if (win->count == 0) {
        return;
    }
    // drop packets that have reached 4 retries
    while (win->count > 0 && win->packet_retries[win->start] >= 4) {
        Packet *pkt = win->window[win->start];
        vPortFree(pkt->pyload);
        vPortFree(pkt);
        win->window[win->start] = NULL;
        win->start = (win->start + 1) % N;
        win->count--;
        win->base++;
        xSemaphoreTake(dropped_mutex, portMAX_DELAY);
        dropped_after_max_retries++;
        if (win->receiver_id == 3) {
            dropCountBySender[0][pkt->sourceID - 1]++;
        } else {
            dropCountBySender[1][pkt->sourceID - 1]++;
        }
        total_dropped++;
        xSemaphoreGive(dropped_mutex);
        printf("Sender %d: Dropped packet seq=%u to dest=%u after 4 retries\n",
               pkt->sourceID, pkt->seq, win->receiver_id);
    }
    // retransmit remaining packets if ther is
    if (win->count > 0) {
        xSemaphoreTake(transmissions_mutex, portMAX_DELAY);
        total_retransmissions += win->count;
        total_transmissions += win->count;
        xSemaphoreGive(transmissions_mutex);

        for (int i = 0; i < win->count; i++) {
            int idx = (win->start + i) % N;
            Packet *pkt = win->window[idx];
            win->packet_retries[idx]++;
            Packet *copy = (Packet *)pvPortMalloc(sizeof(Packet));
            memcpy(copy, pkt, sizeof(Packet));
            copy->pyload = pvPortMalloc(pkt->len - 8);
            memcpy(copy->pyload, pkt->pyload, pkt->len - 8);
            uint32_t tx_delay = transmission_delay_ms(copy->len) + D;
            TimerHandle_t forward_timer = xTimerCreate(
                "FWD", pdMS_TO_TICKS(tx_delay), pdFALSE, copy, forward_data_callback
            );
            xTimerStart(forward_timer, 0);
        }
        xTimerStart(xTimer, 0);
        printf("Timeout for Sender %d to Receiver %u, retransmitted %d packets\n",
               win->window[win->start]->sourceID, win->receiver_id, win->count);
        for (int i = 0; i < win->count; i++) {
            int idx = (win->start + i) % N;
            Packet *pkt = win->window[idx];
            printf("Sender %d: Retransmitted seq=%u to dest=%u, (attempt %d)\n",
                   pkt->sourceID, pkt->seq, win->receiver_id, win->packet_retries[idx]);
        }
    }
}

void forward_data_callback(TimerHandle_t xTimer) {
    Packet *pkt = (Packet *)pvTimerGetTimerID(xTimer);
    int dest_idx = pkt->dest- 3;
    int sender_idx = pkt->sourceID - 1;

    if ((rand() % 100) < P_DROP) {
        printf("Switch: Dropped packet from S%d to R%d, seq=%u due to probability\n",
               pkt->sourceID, pkt->dest, pkt->seq);
        vPortFree(pkt->pyload);
        vPortFree(pkt);
        xSemaphoreTake(dropped_mutex, portMAX_DELAY);
        if (pkt->dest == 3) {
            dropCountBySender[0][sender_idx]++;
        } else {
            dropCountBySender[1][sender_idx]++;
        }
        total_dropped++;
        xSemaphoreGive(dropped_mutex);
    } else {
        QueueHandle_t target_queue = (pkt->dest == 3) ?
            Switch_R3 : Switch_R4;
        if (xQueueSend(target_queue, &pkt, 0) == pdTRUE) {
            printf("Switch: Forwarded Packet S%d → R%d, seq=%u, len=%u\n",
                   pkt->sourceID, pkt->dest, pkt->seq, pkt->len);
        } else {
            printf("Switch: Dropped packet from S%d to R%d, seq=%u due to queue full\n",
                   pkt->sourceID, pkt->dest, pkt->seq);
            vPortFree(pkt->pyload);
            vPortFree(pkt);
            xSemaphoreTake(dropped_mutex, portMAX_DELAY);
            if (pkt->dest == 3) {
                dropCountBySender[0][sender_idx]++;
            } else {
                dropCountBySender[1][sender_idx]++;
            }
            total_dropped++;
            xSemaphoreGive(dropped_mutex);
        }
    }
    xTimerDelete(xTimer, 0);
}

void forward_ack_callback(TimerHandle_t xTimer) {
    Ack *ACK = (Ack *)pvTimerGetTimerID(xTimer);
    if ((rand() % 100) < P_ACK) {
        printf("Switch: Dropped ACK from R%d to S%d, seq=%u \n",
               ACK->senderID, ACK->receiverID, ACK->seq);
        xSemaphoreTake(dropped_mutex, portMAX_DELAY);
        droppedACK++;
        total_dropped++;
        xSemaphoreGive(dropped_mutex);
        vPortFree(ACK);
    } else {
        QueueHandle_t ACK_queue = (ACK->receiverID == 1) ?
            Switch_S1_ACK : Switch_S2_ACK;
        if (xQueueSend(ACK_queue, ACK, 0) == pdTRUE) {
            printf("Switch: Forwarded ACK R%d → S%d, seq=%u\n",
                   ACK->senderID, ACK->receiverID, ACK->seq);
        } else {
            printf("Switch: Dropped ACK from R%d to S%d, seq=%u due to queue full\n",
                   ACK->senderID, ACK->receiverID, ACK->seq);
            xSemaphoreTake(dropped_mutex, portMAX_DELAY);
            droppedACK++;
            total_dropped++;
            xSemaphoreGive(dropped_mutex);
            vPortFree(ACK);
        }
    }
    xTimerDelete(xTimer, 0);
}

void sender_task(void *pvParameters) {
    int sender_id = (int)(intptr_t)pvParameters;
    SenderState *state = &sender_states[sender_id - 1];
    while (1) {
        if (!stop_generating) {
            uint8_t dest = pickDest();
            int win_idx = dest - 3;
            Window *win = &state->windows[win_idx];
            if (win->count < N) {
                Packet *tx_packet = (Packet *)pvPortMalloc(sizeof(Packet));
                tx_packet->sourceID = (uint8_t)sender_id;
                tx_packet->dest = dest;
                tx_packet->seq = win->next_seq;
                int L = randomInRange(L1, L2);
                tx_packet->len = (uint16_t)L;
                tx_packet->pyload = (uint8_t *)pvPortMalloc(L - 8);
                memset(tx_packet->pyload, 0, (L - 8));

                int idx = (win->start + win->count) % N;
                win->window[idx] = tx_packet;
                win->packet_retries[idx] = 0;
                win->count++;
                win->next_seq++;

                Packet *copy = (Packet *)pvPortMalloc(sizeof(Packet));
                memcpy(copy, tx_packet, sizeof(Packet));
                copy->pyload = pvPortMalloc(tx_packet->len - 8);
                memcpy(copy->pyload, tx_packet->pyload, tx_packet->len - 8);
                uint32_t tx_delay = transmission_delay_ms(copy->len) + D;
                TimerHandle_t forward_timer = xTimerCreate(
                    "FWD", pdMS_TO_TICKS(tx_delay), pdFALSE, copy, forward_data_callback
                );
                xTimerStart(forward_timer, 0);

                xSemaphoreTake(transmissions_mutex, portMAX_DELAY);
                total_transmissions++;
                xSemaphoreGive(transmissions_mutex);

                xSemaphoreTake(generated_mutex, portMAX_DELAY);
                total_generated++;
                xSemaphoreGive(generated_mutex);

                printf("Sender %d: Sent Packet to %u, seq=%u, len=%u\n",
                       sender_id, dest, tx_packet->seq, L);

                if (win->count == 1) {
                    xTimerStart(win->timer, 0);
                }
            }
        }
        uint32_t dt = randomInRange(T1, T2);
        vTaskDelay(pdMS_TO_TICKS(dt));
    }
}

void ack_handler_task(void *pvParameters) {
    int sender_id = (int)(intptr_t)pvParameters;
    QueueHandle_t ack_queue = (sender_id == 1) ?
        Switch_S1_ACK : Switch_S2_ACK;
    Ack ACK;
    while (1) {
        if (xQueueReceive(ack_queue, &ACK, portMAX_DELAY) == pdTRUE) {
            int win_idx = ACK.senderID - 3;
            Window *win = &sender_states[sender_id - 1].windows[win_idx];
            uint32_t k = ACK.seq;
            if (k > win->base) {
                uint32_t acked = k - win->base;
                if (acked > win->count) acked = win->count;
                for (uint32_t i = 0; i < acked; i++) {
                    int idx = (win->start + i) % N;
                    Packet *pkt = win->window[idx];
                    vPortFree(pkt->pyload);
                    vPortFree(pkt);
                    win->window[idx] = NULL;
                    win->packet_retries[idx] = 0;
                }
                win->start = (win->start + acked) % N;
                win->count -= acked;
                win->base = k;
                printf("Sender %d: ACK %u received from R%d, window slides to base=%u\n",
                       sender_id, k, ACK.senderID, win->base);
            }
            if (win->count > 0) {
                xTimerReset(win->timer, 0);
            } else {
                xTimerStop(win->timer, 0);
            }
        }
    }
}

void receiver_task(void *pvParameters) {
    int receiver_id = (int)(intptr_t)pvParameters;
    ReceiverSeqState *state = (receiver_id == 3) ? &receiver3_seq_state : &receiver4_seq_state;
    QueueHandle_t data_q = (receiver_id == 3) ? Switch_R3 : Switch_R4;
    QueueHandle_t ack_q = (receiver_id == 3) ? R3_Switch_ACK : R4_Switch_ACK;
    Packet *pkt;
    while (1) {
        if (xQueueReceive(data_q, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (pkt->dest != (uint8_t)receiver_id) {
                printf("Receiver %d: got DATA destined to %u!\n", receiver_id, pkt->dest);
                vPortFree(pkt->pyload);
                vPortFree(pkt);
                continue;
            }
            uint32_t *expected_seq = (pkt->sourceID == 1) ? &state->expected_seq_s1 : &state->expected_seq_s2;
            uint32_t seq = pkt->seq;
            if (seq == *expected_seq) {
                xSemaphoreTake(received_mutex, portMAX_DELAY);
                receiverpcks[pkt->dest-3]++ ;
                receivedBySender[pkt->sourceID-1][pkt->dest-3]++;
                totalByteReceived[pkt->dest-3] += pkt->len;
                    printf("Receiver %d: Received packet from S%d, seq=%u (Total=%u)\n",
                           receiver_id, pkt->sourceID, seq, receiverpcks[pkt->dest-3]++);
                total_received++;
                xSemaphoreGive(received_mutex);
                (*expected_seq)++;
            } else {
                printf("Receiver %d: Discarded packet from S%d, seq=%u, expected=%u\n",
                       receiver_id, pkt->sourceID, seq, *expected_seq);
            }
            Ack *ACK = (Ack *)pvPortMalloc(sizeof(Ack));
            ACK->senderID = (uint8_t)receiver_id;
            ACK->receiverID = pkt->sourceID;
            ACK->seq = *expected_seq;
            vPortFree(pkt->pyload);
            vPortFree(pkt);
            uint32_t tx_delay = transmission_delay_ms(ACK_LENGTH) + D;
            TimerHandle_t ack_timer = xTimerCreate(
                "ACK", pdMS_TO_TICKS(tx_delay), pdFALSE, ACK, forward_ack_callback
            );
            xTimerStart(ack_timer, 0);
        }
    }
}

void monitor_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        xSemaphoreTake(received_mutex, portMAX_DELAY);
        if (receiverpcks[0] >= TARGET_PACKETS_PER_RECEIVER  && receiverpcks[1] >= TARGET_PACKETS_PER_RECEIVER ) {
            stop_generating = pdTRUE;
            if (sender_states[0].windows[0].count == 0 && sender_states[0].windows[1].count == 0 &&
                sender_states[1].windows[0].count == 0 && sender_states[1].windows[1].count == 0) {
                T_end = xTaskGetTickCount();
                uint32_t totalBytesOverall = totalByteReceived[0] + totalByteReceived[1];
                uint32_t period = (T_end - T_start) / 1000;
                uint32_t throughput = (period > 0) ? (totalBytesOverall / period) : 0;
                printf("\n=== FINAL STATS ===\n");
                printf("Simulation time: start time = %u msec & end time = %u msec\n",
                       (unsigned)T_start, (unsigned)T_end);
                printf("Total bytes received by both receivers: %u\n", totalBytesOverall);
                for (int r = 0; r < 2; r++) {
                    printf("Receiver%u Received: %u\n", r + 3, receiverpcks[r]);
                    printf("  Dropped from Sender1: %u\n", dropCountBySender[r][0]);
                    printf("  Dropped from Sender2: %u\n", dropCountBySender[r][1]);
                }
                printf("Total Transmissions: %u\n", total_transmissions); // Note: Shared across senders
                printf("Packets Dropped After Max Retries: %u\n", dropped_after_max_retries); // Shared
                printf("Total Retransmissions: %u\n", total_retransmissions); // Shared
                printf("Total ACKs Dropped: %u\n", droppedACK);
                printf("Throughput: %u bytes/sec\n", throughput);
                printf("===================\n");
                vTaskEndScheduler();
                for (;;) {}
            }
        }
        xSemaphoreGive(received_mutex);
    }
}

int main(void) {
    srand(1234);
    T_start = xTaskGetTickCount();

    // Initialize queues
    S1_Switch = xQueueCreate(10, sizeof(Packet*));
    S2_Switch = xQueueCreate(10, sizeof(Packet*));
    Switch_R3 = xQueueCreate(10, sizeof(Packet*));
    Switch_R4 = xQueueCreate(10, sizeof(Packet*));
    R3_Switch_ACK = xQueueCreate(5, sizeof(Ack));
    R4_Switch_ACK = xQueueCreate(5, sizeof(Ack));
    Switch_S1_ACK = xQueueCreate(5, sizeof(Ack));
    Switch_S2_ACK = xQueueCreate(5, sizeof(Ack));

    if (!S1_Switch || !S2_Switch ||
        !Switch_R3 || !Switch_R4 ||
        !R3_Switch_ACK || !R4_Switch_ACK ||
        !Switch_S1_ACK || !Switch_S2_ACK) {
        printf("Queue creation failed\n");
        for (;;) {}
    }

    received_mutex = xSemaphoreCreateMutex();
    dropped_mutex = xSemaphoreCreateMutex();
    generated_mutex = xSemaphoreCreateMutex();
    transmissions_mutex = xSemaphoreCreateMutex();


    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            Window *win = &sender_states[i].windows[j];
            win->base = 0;
            win->next_seq = 0;
            for (int k = 0; k < N; k++) {
                win->window[k] = NULL;
                win->packet_retries[k] = 0;
            }
            win->start = 0;
            win->count = 0;
            win->receiver_id = 3 + j;
            win->timer = xTimerCreate("Tout", pdMS_TO_TICKS(T_out), pdFALSE, (void *)win, sender_timeout_callback);
            if (win->timer == NULL) {
                printf("Failed to create timer for Sender %d to Receiver %u\n", i + 1, win->receiver_id);
                for(;;);
            }
        }
    }


    xTaskCreate(monitor_task, "Monitor", 2000, NULL, 4, NULL);
    xTaskCreate(receiver_task, "Receiver3", 2000, (void*)(intptr_t)3, 3, NULL);
    xTaskCreate(receiver_task, "Receiver4", 2000, (void*)(intptr_t)4, 3, NULL);
    xTaskCreate(sender_task, "Sender1", 2000, (void*)(intptr_t)1, 2, NULL);
    xTaskCreate(sender_task, "Sender2", 2000, (void*)(intptr_t)2, 2, NULL);
    xTaskCreate(ack_handler_task, "AckHandler1", 2000, (void*)(intptr_t)1, 2, NULL);
    xTaskCreate(ack_handler_task, "AckHandler2", 2000, (void*)(intptr_t)2, 2, NULL);

    vTaskStartScheduler();
    for (;;) {}
    return 0;
}

