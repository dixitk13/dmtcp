#include <string.h>
#include <stdio.h>
#include <unistd.h>    
#include <zookeeper/zookeeper.h>


static zhandle_t *zh;
static int connected;
char gpath[1024];  
static char mycontext[] = "testingdata";
char buffer[255]; 
int master_number = 999;    
char master_string[23];
struct String_vector vector; 
char master_znode_base_path[] = "/master";
char full_master_znode_base_path[] = "/master/";



void watcherforwget(zhandle_t *zzh, int type, int state, const char *path,
             void* context);

void watcher(zhandle_t *zzh, int type, int state, const char *path,
             void* context)                                        
{                                                                  
    if (type == ZOO_SESSION_EVENT) {   
        printf("zoo event changed : watcher \n");                            
        if (state == ZOO_CONNECTED_STATE) {                        
            connected = 1;                                         
        } else if (state == ZOO_AUTH_FAILED_STATE) {               
            zookeeper_close(zzh);                                  
            exit(1);                                               
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {           
            zookeeper_close(zzh);                                  
            exit(1);                                               
        }                                                          

    }   
    printf("init watcher called! \n");
}


void leaderFinding(zhandle_t *zh){
    
    int rc= zoo_get_children(zh, master_znode_base_path, 1, &vector);  
    if(rc != ZOK) { 
     fprintf(stderr,"electLeader: get_children failed\n");  
    } else { 
        int i=0;  
        fprintf(stderr, "checking for coordinators which are available \n");  
        char other_master_string[11];
        
       
        int check_coord_number = -1;
        other_master_string[11] = '\0';

        for(i=0;i<vector.count;i++){  
            fprintf(stderr, "%s\n", vector.data[i]);
            
            strncpy(other_master_string, vector.data[i] + 12, 10);  
            printf("checking other_master node : %s \n", other_master_string);

            // dmtcp_coord_ from dmtcp_coord_0000000001
            check_coord_number = atoi(other_master_string);
            if(master_number > check_coord_number){
                printf("\tupdating master coordinator as %d \n", check_coord_number);
                master_number = check_coord_number;
                strncpy(master_string, vector.data[i], 23);
            }
            printf("\n");           
        }
    }

    fprintf(stderr, "Master determined from znode cluster is  %d with %s \n", master_number, master_string);
    int len = 254; 
    struct Stat st; 
    char full_master_path[30];
    strcpy(full_master_path, full_master_znode_base_path);
    
    strcat(full_master_path, master_string);

    rc = zoo_wget(zh, full_master_path, watcherforwget, mycontext, buffer, &len, &st);

    if(rc != ZOK) {  
        printf("Problems %s %d\n", full_master_path, rc); 
    } else { 
        printf("Data: %s\n", buffer);
        // break here
    }
}

void watcherforwget(zhandle_t *zzh, int type, int state, const char *path,
             void* context)                                               
{                                                                                                                          
    int len, rc;                                                          
    struct Stat st;                                                       
    char *p = (char *)context;                                            
    if (type == ZOO_SESSION_EVENT) {                                      
        if (state == ZOO_CONNECTED_STATE) {                               
            return;                                                       
        } else if (state == ZOO_AUTH_FAILED_STATE) {                      
            zookeeper_close(zzh);                                         
            exit(1);                                                      
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {                  
            zookeeper_close(zzh);                                         
            exit(1);                                                      
        }                                                                 
    } else if (type == ZOO_CHANGED_EVENT) {                               
        printf("Data changed for %s \n", path);                           
        len = 254;                                                        
        //get the changed data and set an watch again                     
        rc = zoo_wget(zh, path, watcherforwget , mycontext, buffer, &len, &st);
        if (ZOK != rc){                                                        
            printf("Problems %s %d\n", path, rc);                              
        } else if (len >= 0) {                                                 
           buffer[len] = 0;                                                    
           printf("Path: %s changed data: %s\n", path, buffer);                
        }                                                                      
    } else if (type == ZOO_DELETED_EVENT ){
        printf("some node was deleted, lets elect leader again! \n");
        
        // before election remove the cached master with us
        master_number = 999;
        // calling leader finding algorithm again
        leaderFinding(zzh);
    }                                                                         
 
    printf("Watcher context %s\n", p);
}  



int main(int argc, char **argv){
    printf("hello zookeeper\n");

    char buffer[512];
    zh = zookeeper_init("localhost:2181", watcher, 30000, 0, 0, 0);

    if(!zh){
        printf("error");
    }

    int buflen = sizeof(buffer);
    struct Stat stat;
    

    leaderFinding(zh);

    
    printf("sleeping! \n");
    // while(1) sleep(1);  
    while(1);  


    fprintf(stderr, "Closing zookeeper\n");  
    
    zookeeper_close(zh);


    return 0;
}


    