// master node checks for existing znode and creates his data

#include <string.h>
#include <stdio.h>
#include <unistd.h>    
#include <zookeeper/zookeeper.h>


static zhandle_t *zh;
static int connected;
char gpath[1024];  
// static char mycontext[] = "testingdata";
char buffer[255]; 
int master_number = 999;	
char master_string[23];
struct String_vector vector; 
static char master_znode_base_path[] = "/master/dmtcp_coord_";


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
c

void create_ephemeral_sequential_node(zhandle_t *zzh){
	printf("creating node, please implement me\n");

	static char value[] = "localhost:9200";

	int rc = zoo_acreate(zzh, master_znode_base_path, value, 14,
    &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL | ZOO_SEQUENCE, NULL, NULL);

    if(rc = ZOK){
    	printf("creating a node with my data \n");
    }
}

//create -s -e /master/dmtcp_coord_ "localhost:1119"
int main(int argc, char **argv){
	printf("hello zookeeper\n");

	char buffer[512];
	zh = zookeeper_init("localhost:2181", watcher, 30000, 0, 0, 0);

	if(!zh){
		printf("error");
	}

	int buflen = sizeof(buffer);
	struct Stat stat;
	

    create_ephemeral_sequential_node(zh);

    
    printf("sleeping! \n");
    // do whatever master does
    while(1) sleep(1);  


	fprintf(stderr, "Closing zookeeper\n");  
	
	zookeeper_close(zh);


	return 0;
}
