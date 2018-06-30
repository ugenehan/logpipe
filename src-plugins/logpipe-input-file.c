#include "logpipe_api.h"

/* cmd for testing
logpipe -f logpipe_case1_collector.conf --start-once-for-env "start_once_for_full_dose 1"
*/

char	*__LOGPIPE_INPUT_FILE_VERSION = "0.3.1" ;

/* 跟踪文件信息结构 */
struct TraceFile
{
	char			*pathname ;
	char			filename[ PATH_MAX + 1 ] ;
	uint16_t		filename_len ;
	char			path_filename[ PATH_MAX + 1 ] ;
	uint16_t		path_filename_len ;
	int			fd ;
	uint64_t		trace_offset ;
	uint64_t		trace_line ;
	uint64_t		rotate_size ;
	
	struct rb_node		inotify_path_filename_rbnode ;
	
	int			inotify_file_wd ;
	struct rb_node		inotify_file_wd_rbnode ;
	
	struct timeval		modifing_timestamp ;
	struct rb_node		modifing_timestamp_rbnode ;
	int			watch_count ;
} ;

/* 目录的IN_MOVED_FROM事件，之后如果没被IN_MOVED_SELF消耗掉，则被清理 */
#define TIME_TO_FREE_ELAPSE	60

struct MovefromEvent
{
	char			filename[ PATH_MAX + 1 ] ;
	struct list_head	movefrom_filename_node ;
	
	uint32_t		cookie ;
	
	time_t			time_to_free ;
} ;

/* 大小转档后的文件，如果长期不变动则清理掉 */
struct RotatedFile
{
	int			inotify_file_wd ;
	struct list_head	rotated_file_node ;
} ;

/* 插件环境结构 */
#define INOTIFY_READ_BUFSIZE	(10*1024*1024)

#define READ_FULL_FILE_FLAG	1
#define READ_APPEND_FILE_FLAG	0

#define APPEND_COUNT_INFINITED	-1

#define WATCH_COUNT_DEFAULT	6

#define CLEAN_TIMEOUT_ROTATED_FILE_ELAPSE	60

#define MAX_USLEEP_INTERVAL_DEFAULT	50000

struct InputPluginContext
{
	char			*path ;
	char			*files ;
	char			*files2 ;
	char			*files3 ;
	char			*exclude_files ;
	char			*exclude_files2 ;
	char			*exclude_files3 ;
	char			exec_before_rotating_buffer[ PATH_MAX * 3 ] ;
	char			*exec_before_rotating ;
	uint64_t		rotate_size ;
	char			exec_after_rotating_buffer[ PATH_MAX * 3 ] ;
	char			*exec_after_rotating ;
	char			*compress_algorithm ;
	int			max_append_count ;
	int			max_watch_count ;
	uint64_t		max_usleep_interval ;
	uint64_t		min_usleep_interval ;
	int			start_once_for_full_dose ;
	
	int			inotify_fd ;
	int			inotify_path_wd ;
	
	struct rb_root		inotify_path_filename_rbtree ;
	struct rb_root		inotify_wd_rbtree ;
	
	struct rb_root		modifing_timestamp_rbtree ;
	struct rb_root		deleting_timestamp_rbtree ;
	
	struct list_head	movefrom_filename_list ;
	struct list_head	rotated_file_list ;
	
	char			*inotify_read_buffer ;
	int			inotify_read_bufsize ;
	
	int			count_to_clean_tmp_events ;
	
	struct timeval		now ;
	struct TraceFile	*p_trace_file ;
	int			fd ;
	uint64_t		remain_len ;
	uint64_t		read_len ;
	uint64_t		read_line ;
	uint64_t		add_len ;
	uint64_t		add_line ;
} ;

LINK_RBTREENODE_STRING( LinkTraceFilePathFilenameTreeNode , struct InputPluginContext , inotify_path_filename_rbtree , struct TraceFile , inotify_path_filename_rbnode , path_filename )
QUERY_RBTREENODE_STRING( QueryTraceFilePathFilenameTreeNode , struct InputPluginContext , inotify_path_filename_rbtree , struct TraceFile , inotify_path_filename_rbnode , path_filename )
UNLINK_RBTREENODE( UnlinkTraceFilePathFilenameTreeNode , struct InputPluginContext , inotify_path_filename_rbtree , struct TraceFile , inotify_path_filename_rbnode )

LINK_RBTREENODE_INT( LinkTraceFileInotifyWdTreeNode , struct InputPluginContext , inotify_wd_rbtree , struct TraceFile , inotify_file_wd_rbnode , inotify_file_wd )
QUERY_RBTREENODE_INT( QueryTraceFileInotifyWdTreeNode , struct InputPluginContext , inotify_wd_rbtree , struct TraceFile , inotify_file_wd_rbnode , inotify_file_wd )
UNLINK_RBTREENODE( UnlinkTraceFileInotifyWdTreeNode , struct InputPluginContext , inotify_wd_rbtree , struct TraceFile , inotify_file_wd_rbnode )

funcCompareRbTreeNodeEntry CompareModifingTimestampRbTreeNode ;
int CompareModifingTimestampRbTreeNode( void *pv1 , void *pv2 )
{
	struct TraceFile	*p_trace_file1 = (struct TraceFile *)pv1 ;
	struct TraceFile	*p_trace_file2 = (struct TraceFile *)pv2 ;
	
	if( p_trace_file1->modifing_timestamp.tv_sec < p_trace_file2->modifing_timestamp.tv_sec )
	{
		return -1;
	}
	else if( p_trace_file1->modifing_timestamp.tv_sec > p_trace_file2->modifing_timestamp.tv_sec )
	{
		return 1;
	}
	else
	{
		if( p_trace_file1->modifing_timestamp.tv_usec < p_trace_file2->modifing_timestamp.tv_usec )
		{
			return -1;
		}
		else if( p_trace_file1->modifing_timestamp.tv_usec > p_trace_file2->modifing_timestamp.tv_usec )
		{
			return 1;
		}
		else
		{
			return 0;
		}
	}
}
LINK_RBTREENODE_ALLOWDUPLICATE( LinkTraceFileModifingTimestampTreeNode , struct InputPluginContext , modifing_timestamp_rbtree , struct TraceFile , modifing_timestamp_rbnode , CompareModifingTimestampRbTreeNode )
UNLINK_RBTREENODE( UnlinkTraceFileModifingTimestampTreeNode , struct InputPluginContext , modifing_timestamp_rbtree , struct TraceFile , modifing_timestamp_rbnode )

void FreeTraceFile( void *pv )
{
	struct TraceFile      *p_trace_file = (struct TraceFile *) pv ;
	
	if( p_trace_file )
	{
		/* 关闭文件 */
		if( p_trace_file->fd >= 0 )
		{
			INFOLOGC( "close[%d] ok , path_filename[%s]" , p_trace_file->fd , p_trace_file->path_filename )
			close( p_trace_file->fd ); p_trace_file->fd = -1 ;
		}
		
		/* 释放堆内存 */
		free( p_trace_file );
		p_trace_file = NULL ;
	}
	
	return;
}
DESTROY_RBTREE( DestroyTraceFileTree , struct InputPluginContext , inotify_wd_rbtree , struct TraceFile , inotify_file_wd_rbnode , FreeTraceFile )

/* 日志文件大小转档 */
static int RotatingFile( struct InputPluginContext *p_plugin_ctx , struct TraceFile *p_trace_file )
{
	struct timeval		tv ;
	struct tm		tm ;
	char			old_filename[ PATH_MAX + 1 ] ;
	char			new_filename[ PATH_MAX + 1 ] ;
	
	struct RotatedFile	*p_rotated_file = NULL ;
	
	int			nret = 0 ;
	
	/* 组装转档后文件名 */
	snprintf( old_filename , sizeof(old_filename)-1 , "%s/%s" , p_trace_file->pathname , p_trace_file->filename );
	gettimeofday( & tv , NULL );
	memset( & tm , 0x00 , sizeof(struct tm) );
	localtime_r( & (tv.tv_sec) , & tm );
	memset( new_filename , 0x00 , sizeof(new_filename) );
	snprintf( new_filename , sizeof(new_filename)-1 , "%s/_%s-%04d%02d%02d_%02d%02d%02d_%06ld" , p_trace_file->pathname , p_trace_file->filename , tm.tm_year+1900 , tm.tm_mon+1 , tm.tm_mday , tm.tm_hour , tm.tm_min , tm.tm_sec , tv.tv_usec );
	
	/* 设置环境变量 */
	setenv( "LOGPIPE_ROTATING_PATHNAME" , p_trace_file->pathname , 1 );
	setenv( "LOGPIPE_ROTATING_OLD_FILENAME" , old_filename , 1 );
	setenv( "LOGPIPE_ROTATING_NEW_FILENAME" , new_filename , 1 );
	
	/* 执行转档前命令 */
	if( p_plugin_ctx->exec_before_rotating && p_plugin_ctx->exec_before_rotating[0] )
	{
		system( p_plugin_ctx->exec_before_rotating );
	}
	
	/* 文件改名转档 */
	nret = rename( old_filename , new_filename ) ;
	
	/* 执行转档后命令 */
	if( p_plugin_ctx->exec_after_rotating && p_plugin_ctx->exec_after_rotating[0] )
	{
		system( p_plugin_ctx->exec_after_rotating );
	}
	
	/* 判断改名是否成功 */
	if( nret )
	{
		FATALLOGC( "rename [%s] to [%s] failed , errno[%d]" , old_filename , new_filename , errno )
		return -1;
	}
	else
	{
		INFOLOGC( "rename [%s] to [%s] ok" , old_filename , new_filename )
	}
	
	strncpy( p_trace_file->path_filename , new_filename , sizeof(p_trace_file->path_filename)-1 );
	
	/* 注册大小转档文件信息 */
	p_rotated_file = (struct RotatedFile *)malloc( sizeof(struct RotatedFile) ) ;
	if( p_rotated_file == NULL )
	{
		ERRORLOGC( "malloc sizeof(struct RotatedFile) failed , errno[%d]" , errno )
	}
	else
	{
		memset( p_rotated_file , 0x00 , sizeof(struct RotatedFile) ) ;
		p_rotated_file->inotify_file_wd = p_trace_file->inotify_file_wd ;
		list_add_tail( & (p_rotated_file->rotated_file_node) , & (p_plugin_ctx->rotated_file_list) );
		INFOLOGC( "add rotated file ok , inotify_wd[%d] path_filename[%s] filename[%s]" , p_rotated_file->inotify_file_wd , p_trace_file->path_filename , p_trace_file->filename )
	}
	
	/* 大小转档过的文件无需再转档 */
	p_trace_file->rotate_size = 0 ;
	
	return 0;
}

/* 清除文件变化监视器 */
static int RemoveFileWatcher( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx , struct TraceFile *p_trace_file )
{
	int		nret = 0 ;
	
	UnlinkTraceFilePathFilenameTreeNode( p_plugin_ctx , p_trace_file );
	UnlinkTraceFileInotifyWdTreeNode( p_plugin_ctx , p_trace_file );
	UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
	
	nret = inotify_rm_watch( p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd ) ;
	if( nret == -1 )
	{
		ERRORLOGC( "inotify_rm_watch failed[%d] , inotify_fd[%d] inotify_wd[%d] path_filename[%s]" , nret , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	else
	{
		INFOLOGC( "inotify_rm_watch ok , inotify_fd[%d] inotify_wd[%d] path_filename[%s]" , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	
	FreeTraceFile( p_trace_file );
	
	return 0;
}

static void SendCloseWriteEvent( struct TraceFile *p_trace_file )
{
	int		fd ;
	
	fd = open( p_trace_file->path_filename , O_WRONLY|O_APPEND ) ;
	if( fd >= 0 )
		close( fd );
	
	return;
}

/* 检查文件最后偏移量 */
static int CheckFileOffset( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx , struct TraceFile *p_trace_file , uint64_t rotate_size , uint64_t max_append_count )
{
	struct stat		file_stat ;
	uint64_t		append_count ;
	
	int			nret = 0 ;
	
	DEBUGLOGC( "CheckFileOffset path_filename[%s] filename[%s]" , p_trace_file->path_filename , p_trace_file->filename )
	
	/* 获得文件大小 */
	memset( & file_stat , 0x00 , sizeof(struct stat) );
	nret = fstat( p_trace_file->fd , & file_stat ) ;
	if( nret == -1 )
	{
		ERRORLOGC( "fstat[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
	}
	
	append_count = 0 ;
	
_GOTO_WRITEALLOUTPUTPLUGINS :
	
	/* 如果文件大小 小于 跟踪的文件大小 */
	if( file_stat.st_size < p_trace_file->trace_offset )
	{
		INFOLOGC( "file_size[%"PRIu64"] < trace_offset[%"PRIu64"]" , file_stat.st_size , p_trace_file->trace_offset )
		p_trace_file->trace_offset = file_stat.st_size ;
		
		p_trace_file->watch_count = 1 ;
		UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
		p_trace_file->modifing_timestamp.tv_sec = time(NULL) + (int)pow(2,(double)(p_trace_file->watch_count)) ;
		p_trace_file->modifing_timestamp.tv_usec = 0 ;
		LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
		DEBUGLOGC( "set watch_count[%d] timestamp[%ld]" , p_trace_file->watch_count , p_trace_file->modifing_timestamp.tv_sec )
	}
	/* 如果文件大小 大于 跟踪的文件大小 */
	else if( file_stat.st_size > p_trace_file->trace_offset )
	{
		INFOLOGC( "file_size[%"PRIu64"] > trace_offset[%"PRIu64"]" , file_stat.st_size , p_trace_file->trace_offset )
		
		lseek( p_trace_file->fd , p_trace_file->trace_offset , SEEK_SET );
		
		/* 激活一轮从输入插件读，写到所有输出插件流程处理 */
		p_plugin_ctx->p_trace_file = p_trace_file ;
		p_plugin_ctx->fd = p_trace_file->fd ;
		p_plugin_ctx->remain_len = file_stat.st_size - p_trace_file->trace_offset ;
		nret = WriteAllOutputPlugins( p_env , p_logpipe_input_plugin , p_trace_file->filename_len , p_trace_file->filename ) ;
		if( nret < 0 )
		{
			ERRORLOGC( "WriteAllOutputPlugins failed[%d]" , nret )
			return -1;
		}
		else if( nret > 0 )
		{
			WARNLOGC( "WriteAllOutputPlugins return[%d]" , nret )
			return 0;
		}
		else
		{
			INFOLOGC( "WriteAllOutputPlugins ok" )
		}
		
		/* 再次获得文件大小 */
		memset( & file_stat , 0x00 , sizeof(struct stat) );
		nret = fstat( p_trace_file->fd , & file_stat ) ;
		if( nret == -1 )
		{
			ERRORLOGC( "fstat[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
			return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
		}
		
		if( file_stat.st_size > p_trace_file->trace_offset ) /* 采集过程中文件还在变大 */
		{
			/* 如果启用文件追读，再处理一个数据块 */
			if( max_append_count >= 0 )
			{
				append_count++;
				if( append_count <= max_append_count )
				{
					goto _GOTO_WRITEALLOUTPUTPLUGINS;
				}
				else
				{
					INFOLOGC( "file_size[%"PRIu64"] >> trace_offset[%"PRIu64"] , rotate_size[%"PRIu64"]" , file_stat.st_size , p_trace_file->trace_offset , p_trace_file->rotate_size )
					SendCloseWriteEvent( p_trace_file );
				}
			}
			else if( max_append_count == APPEND_COUNT_INFINITED )
			{
				goto _GOTO_WRITEALLOUTPUTPLUGINS;
			}
		}
		else
		{
			p_trace_file->watch_count = 1 ;
			UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			p_trace_file->modifing_timestamp.tv_sec = time(NULL) + (int)pow(2,(double)(p_trace_file->watch_count)) ;
			p_trace_file->modifing_timestamp.tv_usec = 0 ;
			LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			DEBUGLOGC( "set watch_count[%d] timestamp[%ld]" , p_trace_file->watch_count , p_trace_file->modifing_timestamp.tv_sec )
		}
	}
	else
	{
		INFOLOGC( "file_size[%"PRIu64"] == trace_offset[%"PRIu64"]" , file_stat.st_size , p_trace_file->trace_offset )
		
		p_trace_file->watch_count++;
		if( p_trace_file->watch_count <= p_plugin_ctx->max_watch_count )
		{
			UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			p_trace_file->modifing_timestamp.tv_sec = time(NULL) + (int)pow(2,(double)(p_trace_file->watch_count)) ;
			p_trace_file->modifing_timestamp.tv_usec = 0 ;
			LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			DEBUGLOGC( "set watch_count[%d] timestamp[%ld]" , p_trace_file->watch_count , p_trace_file->modifing_timestamp.tv_sec )
		}
		else
		{
			p_trace_file->watch_count = 0 ;
			
			UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			p_trace_file->modifing_timestamp.tv_sec = 0 ;
			p_trace_file->modifing_timestamp.tv_usec = 0 ;
			LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
			DEBUGLOGC( "unset watch timestamp" )
		}
	}
	
	/* 如果启用大小转档，且文件有追加内容 */
	if( p_trace_file->rotate_size > 0 && file_stat.st_size >= p_trace_file->rotate_size )
	{
		/* 大小转档文件 */
		nret = RotatingFile( p_plugin_ctx , p_trace_file ) ;
		if( nret )
		{
			ERRORLOGC( "RotatingFile failed[%d] , path_filename[%s]" , nret , p_trace_file->path_filename )
		}
		else
		{
			INFOLOGC( "RotatingFile ok , path_filename[%s]" , p_trace_file->path_filename )
		}
	}
	
	return 0;
}

/* 识别通配表达式 */
static int IsMatchString(char *pcMatchString, char *pcObjectString, char cMatchMuchCharacters, char cMatchOneCharacters)
{
	int el=strlen(pcMatchString);
	int sl=strlen(pcObjectString);
	char cs,ce;

	int is,ie;
	int last_xing_pos=-1;

	for(is=0,ie=0;is<sl && ie<el;){
		cs=pcObjectString[is];
		ce=pcMatchString[ie];

		if(cs!=ce){
			if(ce==cMatchMuchCharacters){
				last_xing_pos=ie;
				ie++;
			}else if(ce==cMatchOneCharacters){
				is++;
				ie++;
			}else if(last_xing_pos>=0){
				while(ie>last_xing_pos){
					ce=pcMatchString[ie];
					if(ce==cs)
						break;
					ie--;
				}

				if(ie==last_xing_pos)
					is++;
			}else
				return -1;
		}else{
			is++;
			ie++;
		}
	}

	if(pcObjectString[is]==0 && pcMatchString[ie]==0)
		return 0;

	if(pcMatchString[ie]==0)
		ie--;

	if(ie>=0){
		while(pcMatchString[ie])
			if(pcMatchString[ie++]!=cMatchMuchCharacters)
				return -2;
	} 

	return 0;
}

/* 统计内存块中某字节出现的次数 */
static uint64_t stat_memchr( char *s , size_t n , char c )
{
	char		*p = NULL ;
	char		*end = NULL ;
	uint64_t	count ;
	
	if( s == NULL || n <= 0 )
		return 0;
	
	for( p=s , end=s+n-1 , count=0 ; p <= end ; p++ )
	{
		if( (*p) == c )
			count++;
	}
	
	return count;
}

/* 统计文件快中某字节出现的次数 */
static uint64_t stat_filechr( char *pathfilename , size_t n , char c )
{
	int		fd ;
	char		*base = NULL ;
	uint64_t	count ;
	
	fd = open( pathfilename , O_RDONLY ) ;
	if( fd == -1 )
		return -1;
	
	base = mmap( NULL , n , PROT_READ , MAP_PRIVATE , fd , 0 ) ;
	close( fd );
	if( base == MAP_FAILED )
		return -3;
	
	count = stat_memchr( base , n , c ) ;
	
	munmap( base , n );
	
	return count;
}

/* 添加文件变化监视器 */
static int AddFileWatcher( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx , char *filename , unsigned char read_full_file_flag , uint64_t rotate_size , uint64_t max_append_count )
{
	struct TraceFile	*p_trace_file = NULL ;
	struct stat		file_stat ;
	unsigned char		survive_flag ;
	
	int			nret = 0 ;
	
	/* 不处理文件名以'.'或'_'开头的文件 */
	if( filename[0] == '.' || filename[0] == '_' )
	{
		DEBUGLOGC( "filename[%s] ignored" , filename )
		return 0;
	}
	
	/* 不处理文件名白名单以外的文件 */
	survive_flag = 1 ;
	
	if( p_plugin_ctx->files && p_plugin_ctx->files[0] )
	{
		if( IsMatchString( p_plugin_ctx->files , filename , '*' , '?' ) != 0 )
		{
			DEBUGLOGC( "filename[%s] not match files[%s]" , filename , p_plugin_ctx->files )
			survive_flag = 0 ;
		}
	}
	
	if( p_plugin_ctx->files2 && p_plugin_ctx->files2[0] )
	{
		if( IsMatchString( p_plugin_ctx->files2 , filename , '*' , '?' ) != 0 )
		{
			DEBUGLOGC( "filename[%s] not match files[%s]" , filename , p_plugin_ctx->files2 )
			survive_flag = 0 ;
		}
	}
	
	if( p_plugin_ctx->files3 && p_plugin_ctx->files3[0] )
	{
		if( IsMatchString( p_plugin_ctx->files3 , filename , '*' , '?' ) != 0 )
		{
			DEBUGLOGC( "filename[%s] not match files[%s]" , filename , p_plugin_ctx->files3 )
			survive_flag = 0 ;
		}
	}
	
	if( survive_flag == 0 )
		return 0;
	
	/* 不处理文件名黑名单以内的文件 */
	if( p_plugin_ctx->exclude_files && p_plugin_ctx->exclude_files[0] )
	{
		if( IsMatchString( p_plugin_ctx->exclude_files , filename , '*' , '?' ) == 0 )
		{
			DEBUGLOGC( "filename[%s] match exclude_files[%s]" , filename , p_plugin_ctx->exclude_files )
			return 0;
		}
	}
	
	if( p_plugin_ctx->exclude_files2 && p_plugin_ctx->exclude_files2[0] )
	{
		if( IsMatchString( p_plugin_ctx->exclude_files2 , filename , '*' , '?' ) == 0 )
		{
			DEBUGLOGC( "filename[%s] match exclude_files[%s]" , filename , p_plugin_ctx->exclude_files2 )
			return 0;
		}
	}
	
	if( p_plugin_ctx->exclude_files3 && p_plugin_ctx->exclude_files3[0] )
	{
		if( IsMatchString( p_plugin_ctx->exclude_files3 , filename , '*' , '?' ) == 0 )
		{
			DEBUGLOGC( "filename[%s] match exclude_files[%s]" , filename , p_plugin_ctx->exclude_files3 )
			return 0;
		}
	}
	
	/* 分配内存以构建文件跟踪结构 */
	p_trace_file = (struct TraceFile *)malloc( sizeof(struct TraceFile) ) ;
	if( p_trace_file == NULL )
	{
		ERRORLOGC( "malloc failed , errno[%d]" , errno )
		return -1;
	}
	memset( p_trace_file , 0x00 , sizeof(struct TraceFile) );
	p_trace_file->fd = -1 ;
	
	/* 填充文件跟踪结构 */
	p_trace_file->pathname = p_plugin_ctx->path ;
	p_trace_file->filename_len = snprintf( p_trace_file->filename , sizeof(p_trace_file->filename)-1 , "%s" , filename ) ;
	if( SNPRINTF_OVERFLOW( p_trace_file->filename_len , sizeof(p_trace_file->filename) ) )
	{
		ERRORLOGC( "snprintf[%s] overflow" , filename )
		FreeTraceFile( p_trace_file );
		return -1;
	}
	p_trace_file->path_filename_len = snprintf( p_trace_file->path_filename , sizeof(p_trace_file->path_filename)-1 , "%s/%s" , p_plugin_ctx->path , filename ) ;
	if( SNPRINTF_OVERFLOW( p_trace_file->filename_len , sizeof(p_trace_file->filename) ) )
	{
		ERRORLOGC( "snprintf[%s] overflow" , filename )
		FreeTraceFile( p_trace_file );
		return -1;
	}
	
	p_trace_file->fd = open( p_trace_file->path_filename , O_RDONLY ) ;
	if( p_trace_file->fd == -1 )
	{
		ERRORLOGC( "open[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		FreeTraceFile( p_trace_file );
		return -1;
	}
	else
	{
		INFOLOGC( "open[%s] ok , fd[%d]" , p_trace_file->path_filename , p_trace_file->fd )
	}
	
	nret = fstat( p_trace_file->fd , & file_stat ) ;
	if( nret == -1 )
	{
		WARNLOGC( "file[%s] not found" , p_trace_file->path_filename )
		close( p_trace_file->fd );
		FreeTraceFile( p_trace_file );
		return 0;
	}
	
	if( ! S_ISREG(file_stat.st_mode) )
	{
		INFOLOGC( "[%s] is not a file" , p_trace_file->path_filename )
		close( p_trace_file->fd );
		FreeTraceFile( p_trace_file );
		return 0;
	}
	
	if( read_full_file_flag == READ_FULL_FILE_FLAG )
	{
		p_trace_file->trace_offset = 0 ;
		p_trace_file->trace_line = 1 ;
	}
	else
	{
		p_trace_file->trace_offset = file_stat.st_size ;
		p_trace_file->trace_line = 1 + stat_filechr( p_trace_file->path_filename , file_stat.st_size , '\n' ) ;
	}
	
	p_trace_file->rotate_size = p_plugin_ctx->rotate_size ;
	
	/* 添加文件变化监视器 */
	p_trace_file->inotify_file_wd = inotify_add_watch( p_plugin_ctx->inotify_fd , p_trace_file->path_filename , (uint32_t)(IN_MODIFY|IN_CLOSE_WRITE|IN_DELETE_SELF|IN_IGNORED|IN_Q_OVERFLOW) ) ;
	if( p_trace_file->inotify_file_wd == -1 )
	{
		ERRORLOGC( "inotify_add_watch[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		close( p_trace_file->fd );
		FreeTraceFile( p_trace_file );
		return -1;
	}
	else
	{
		INFOLOGC( "inotify_add_watch[%s] ok , inotify_fd[%d] inotify_wd[%d] trace_offset[%"PRIu64"] trace_line[%"PRIu64"]" , p_trace_file->path_filename , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->trace_offset , p_trace_file->trace_line )
	}
	
	nret = LinkTraceFilePathFilenameTreeNode( p_plugin_ctx , p_trace_file ) ;
	if( nret )
	{
		ERRORLOGC( "LinkTraceFilePathFilenameTreeNode failed[%d] , wd[%d] path_filename[%s]" , nret , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
		INFOLOGC( "inotify_rm_watch[%s] , inotify_fd[%d] inotify_wd[%d]" , p_trace_file->path_filename , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd )
		inotify_rm_watch( p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd );
		close( p_trace_file->fd );
		FreeTraceFile( p_trace_file );
		return -1;
	}
	else
	{
		DEBUGLOGC( "LinkTraceFileInotifyWdTreeNode ok , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	
	nret = LinkTraceFileInotifyWdTreeNode( p_plugin_ctx , p_trace_file ) ;
	if( nret )
	{
		ERRORLOGC( "LinkTraceFileInotifyWdTreeNode failed[%d] , wd[%d] path_filename[%s]" , nret , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
		UnlinkTraceFilePathFilenameTreeNode( p_plugin_ctx , p_trace_file );
		INFOLOGC( "inotify_rm_watch[%s] , inotify_fd[%d] inotify_wd[%d]" , p_trace_file->path_filename , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd )
		inotify_rm_watch( p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd );
		close( p_trace_file->fd );
		FreeTraceFile( p_trace_file );
		return -1;
	}
	else
	{
		DEBUGLOGC( "LinkTraceFileInotifyWdTreeNode ok , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	
	gettimeofday( & (p_trace_file->modifing_timestamp) , NULL );
	LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
	p_trace_file->watch_count = 0 ;
	
	/* 检查文件变动 */
	nret = CheckFileOffset( p_env , p_logpipe_input_plugin, p_plugin_ctx , p_trace_file , rotate_size , max_append_count ) ;
	if( nret )
	{
		ERRORLOGC( "CheckFileOffset failed[%d] , wd[%d] path_filename[%s]" , nret , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	else
	{
		INFOLOGC( "CheckFileOffset ok , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
	}
	
	return 0;
}

/* 启动时把现存文件都设置变化监视器 */
static int ReadFilesToinotifyWdTree( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx )
{
	DIR			*dir = NULL ;
	struct dirent		*ent = NULL ;
	
	int			nret = 0 ;
	
	/* 打开目录 */
	dir = opendir( p_plugin_ctx->path ) ;
	if( dir == NULL )
	{
		ERRORLOGC( "opendir[%s] failed , errno[%d]\n" , p_plugin_ctx->path , errno )
		return -1;
	}
	
	while(1)
	{
		/* 遍历目录中的所有文件 */
		ent = readdir( dir ) ;
		if( ent == NULL )
			break;
		
		if( ent->d_type & DT_REG )
		{
			nret = AddFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , ent->d_name , (p_plugin_ctx->start_once_for_full_dose?READ_FULL_FILE_FLAG:READ_APPEND_FILE_FLAG) , p_plugin_ctx->rotate_size , p_plugin_ctx->max_append_count ) ;
			if( nret )
			{
				ERRORLOGC( "AddFileWatcher[%s] failed[%d]" , ent->d_name , nret )
				closedir( dir );
				return -1;
			}
		}
	}
	
	/* 打开目录 */
	closedir( dir );
	
	return 0;
}

funcLoadInputPluginConfig LoadInputPluginConfig ;
int LoadInputPluginConfig( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct LogpipePluginConfigItem *p_plugin_config_items , void **pp_context )
{
	struct InputPluginContext	*p_plugin_ctx = NULL ;
	char				*p = NULL ;
	
	/* 申请内存以存放插件上下文 */
	p_plugin_ctx = (struct InputPluginContext *)malloc( sizeof(struct InputPluginContext) ) ;
	if( p_plugin_ctx == NULL )
	{
		ERRORLOGC( "malloc failed , errno[%d]" , errno )
		return -1;
	}
	memset( p_plugin_ctx , 0x00 , sizeof(struct InputPluginContext) );
	
	/* 解析插件配置 */
	p_plugin_ctx->path = QueryPluginConfigItem( p_plugin_config_items , "path" ) ;
	INFOLOGC( "path[%s]" , p_plugin_ctx->path )
	if( p_plugin_ctx->path == NULL || p_plugin_ctx->path[0] == '\0' )
	{
		ERRORLOGC( "expect config for 'path'" )
		return -1;
	}
	
	p_plugin_ctx->files = QueryPluginConfigItem( p_plugin_config_items , "files" ) ;
	INFOLOGC( "files[%s]" , p_plugin_ctx->files )
	
	p_plugin_ctx->files2 = QueryPluginConfigItem( p_plugin_config_items , "files2" ) ;
	INFOLOGC( "files2[%s]" , p_plugin_ctx->files2 )
	
	p_plugin_ctx->files3 = QueryPluginConfigItem( p_plugin_config_items , "files3" ) ;
	INFOLOGC( "files3[%s]" , p_plugin_ctx->files3 )
	
	p_plugin_ctx->exclude_files = QueryPluginConfigItem( p_plugin_config_items , "exclude_files" ) ;
	INFOLOGC( "exclude_files[%s]" , p_plugin_ctx->exclude_files )
	
	p_plugin_ctx->exclude_files2 = QueryPluginConfigItem( p_plugin_config_items , "exclude_files2" ) ;
	INFOLOGC( "exclude_files2[%s]" , p_plugin_ctx->exclude_files2 )
	
	p_plugin_ctx->exclude_files3 = QueryPluginConfigItem( p_plugin_config_items , "exclude_files3" ) ;
	INFOLOGC( "exclude_files3[%s]" , p_plugin_ctx->exclude_files3 )
	
	p = QueryPluginConfigItem( p_plugin_config_items , "exec_before_rotating" ) ;
	if( p )
	{
		int		buffer_len = 0 ;
		int		remain_len = sizeof(p_plugin_ctx->exec_before_rotating_buffer)-1 ;
		
		memset( p_plugin_ctx->exec_before_rotating_buffer , 0x00 , sizeof(p_plugin_ctx->exec_before_rotating_buffer) );
		JSONUNESCAPE_FOLD( p , strlen(p) , p_plugin_ctx->exec_before_rotating_buffer , buffer_len , remain_len )
		if( buffer_len == -1 )
		{
			ERRORLOGC( "exec_before_rotating[%s] invalid" , p )
			return -1;
		}
		
		p_plugin_ctx->exec_before_rotating = p_plugin_ctx->exec_before_rotating_buffer ;
	}
	INFOLOGC( "exec_before_rotating[%s]" , p_plugin_ctx->exec_before_rotating )
	
	p = QueryPluginConfigItem( p_plugin_config_items , "rotate_size" ) ;
	if( p )
		p_plugin_ctx->rotate_size = size64_atou64(p) ;
	else
		p_plugin_ctx->rotate_size = 0 ;
	INFOLOGC( "rotate_size[%ld]" , p_plugin_ctx->rotate_size )
	if( p_plugin_ctx->rotate_size == UINT64_MAX )
	{
		ERRORLOGC( "rotate_size[%"PRIu64"] invalid" , p_plugin_ctx->rotate_size )
		return -1;
	}
	
	p = QueryPluginConfigItem( p_plugin_config_items , "exec_after_rotating" ) ;
	if( p )
	{
		int		buffer_len = 0 ;
		int		remain_len = sizeof(p_plugin_ctx->exec_after_rotating_buffer)-1 ;
		
		memset( p_plugin_ctx->exec_after_rotating_buffer , 0x00 , sizeof(p_plugin_ctx->exec_after_rotating_buffer) );
		JSONUNESCAPE_FOLD( p , strlen(p) , p_plugin_ctx->exec_after_rotating_buffer , buffer_len , remain_len )
		if( buffer_len == -1 )
		{
			ERRORLOGC( "exec_after_rotating[%s] invalid" , p )
			return -1;
		}
		
		p_plugin_ctx->exec_after_rotating = p_plugin_ctx->exec_after_rotating_buffer ;
	}
	INFOLOGC( "exec_after_rotating[%s]" , p_plugin_ctx->exec_after_rotating )
	
	p_plugin_ctx->compress_algorithm = QueryPluginConfigItem( p_plugin_config_items , "compress_algorithm" ) ;
	if( p_plugin_ctx->compress_algorithm )
	{
		if( STRCMP( p_plugin_ctx->compress_algorithm , == , "deflate" ) )
		{
			;
		}
		else
		{
			ERRORLOGC( "compress_algorithm[%s] invalid" , p_plugin_ctx->compress_algorithm )
			return -1;
		}
	}
	INFOLOGC( "compress_algorithm[%s]" , p_plugin_ctx->compress_algorithm )
	
	p = QueryPluginConfigItem( p_plugin_config_items , "max_append_count" ) ;
	if( p )
		p_plugin_ctx->max_append_count = size64_atou64(p) ;
	else
		p_plugin_ctx->max_append_count = 0 ;
	if( p_plugin_ctx->max_append_count == UINT64_MAX )
		p_plugin_ctx->max_append_count = 0 ;
	INFOLOGC( "max_append_count[%"PRIu64"]" , p_plugin_ctx->max_append_count )
	
	p = QueryPluginConfigItem( p_plugin_config_items , "max_watch_count" ) ;
	if( p )
		p_plugin_ctx->max_watch_count = size64_atou64(p) ;
	else
		p_plugin_ctx->max_watch_count = 0 ;
	if( p_plugin_ctx->max_watch_count == UINT64_MAX || p_plugin_ctx->max_watch_count <= 0 )
		p_plugin_ctx->max_watch_count = WATCH_COUNT_DEFAULT ;
	INFOLOGC( "max_watch_count[%"PRIu64"]" , p_plugin_ctx->max_watch_count )
	
	p = QueryPluginConfigItem( p_plugin_config_items , "max_usleep_interval" ) ;
	if( p )
		p_plugin_ctx->max_usleep_interval = usleep_atou64(p) ;
	else
		p_plugin_ctx->max_usleep_interval = 0 ;
	INFOLOGC( "max_usleep_interval[%"PRIu64"]" , p_plugin_ctx->max_usleep_interval )
	if( p_plugin_ctx->max_usleep_interval == UINT64_MAX )
	{
		ERRORLOGC( "max_usleep_interval[%"PRIu64"] invalid" , p_plugin_ctx->max_usleep_interval )
		return -1;
	}
	
	p = QueryPluginConfigItem( p_plugin_config_items , "min_usleep_interval" ) ;
	if( p )
		p_plugin_ctx->min_usleep_interval = usleep_atou64(p) ;
	else
		p_plugin_ctx->min_usleep_interval = 0 ;
	INFOLOGC( "min_usleep_interval[%"PRIu64"]" , p_plugin_ctx->min_usleep_interval )
	if( p_plugin_ctx->min_usleep_interval == UINT64_MAX )
	{
		ERRORLOGC( "max_usleep_interval[%"PRIu64"] invalid" , p_plugin_ctx->min_usleep_interval )
		return -1;
	}
	
	/* 设置插件环境上下文 */
	(*pp_context) = p_plugin_ctx ;
	
	return 0;
}

funcInitInputPluginContext InitInputPluginContext ;
int InitInputPluginContext( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)(p_context) ;
	char				*p = NULL ;
	
	int				nret = 0 ;
	
	/* 补充从环境变量中解析配置 */
	p = getenv( "start_once_for_full_dose" ) ;
	if( p )
		p_plugin_ctx->start_once_for_full_dose = 1 ;
	else
		p_plugin_ctx->start_once_for_full_dose = 0 ;
	INFOLOGC( "start_once_for_full_dose[%d]" , p_plugin_ctx->start_once_for_full_dose )
	
	/* 初始化其它变量 */
	INIT_LIST_HEAD( & (p_plugin_ctx->movefrom_filename_list) );
	INIT_LIST_HEAD( & (p_plugin_ctx->rotated_file_list) );
	
	/* 初始化插件环境内部数据 */
	p_plugin_ctx->inotify_fd = inotify_init() ;
	if( p_plugin_ctx->inotify_fd == -1 )
	{
		ERRORLOGC( "inotify_init failed , errno[%d]" , errno )
		return -1;
	}
	
	p_plugin_ctx->inotify_path_wd = inotify_add_watch( p_plugin_ctx->inotify_fd , p_plugin_ctx->path , (uint32_t)(IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE_SELF|IN_MOVE_SELF|IN_Q_OVERFLOW) );
	if( p_plugin_ctx->inotify_path_wd == -1 )
	{
		ERRORLOGC( "inotify_add_watch[%s] failed , errno[%d]" , p_plugin_ctx->path , errno )
		return -1;
	}
	else
	{
		INFOLOGC( "inotify_add_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_plugin_ctx->path , p_plugin_ctx->inotify_fd , p_plugin_ctx->inotify_path_wd )
	}
	
	p_plugin_ctx->inotify_read_bufsize = INOTIFY_READ_BUFSIZE ;
	p_plugin_ctx->inotify_read_buffer = (char*)malloc( p_plugin_ctx->inotify_read_bufsize ) ;
	if( p_plugin_ctx->inotify_read_buffer == NULL )
	{
		ERRORLOGC( "malloc failed , errno[%d]" , errno )
		return -1;
	}
	memset( p_plugin_ctx->inotify_read_buffer , 0x00 , p_plugin_ctx->inotify_read_bufsize );
	
	/* 装载现存文件 */
	nret = ReadFilesToinotifyWdTree( p_env , p_logpipe_input_plugin , p_plugin_ctx ) ;
	if( nret )
	{
		return nret;
	}
	
	/* 设置输入描述字 */
	AddInputPluginEvent( p_env , p_logpipe_input_plugin , p_plugin_ctx->inotify_fd );
	
	return 0;
}

static int ProcessingRenameFileEvent( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx , struct inotify_event *p_inotify_event )
{
	struct MovefromEvent	*p_curr_movefrom_filename = NULL ;
	struct MovefromEvent	*p_next_movefrom_filename = NULL ;
	struct TraceFile	trace_file ;
	struct TraceFile	*p_trace_file = NULL ;
	
	int			nret = 0 ;
	
	list_for_each_entry_safe( p_curr_movefrom_filename , p_next_movefrom_filename , & (p_plugin_ctx->movefrom_filename_list) , struct MovefromEvent , movefrom_filename_node )
	{
		if( p_curr_movefrom_filename->cookie == p_inotify_event->cookie )
		{
			INFOLOGC( "find movefrom_filename_node , cookie[%d] , filename[%s]" , p_curr_movefrom_filename->cookie , p_curr_movefrom_filename->filename )
			
			memset( & trace_file , 0x00 , sizeof(struct TraceFile) );
			snprintf( trace_file.path_filename , sizeof(trace_file.path_filename)-1 , "%s/%s" , p_plugin_ctx->path , p_curr_movefrom_filename->filename );
			p_trace_file = QueryTraceFilePathFilenameTreeNode( p_plugin_ctx , & trace_file ) ;
			if( p_trace_file )
			{
				nret = inotify_rm_watch( p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd ) ;
				if( nret == -1 )
				{
					ERRORLOGC( "inotify_rm_watch failed[%d] , inotify_fd[%d] inotify_wd[%d] path_filename[%s]" , nret , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
					RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
				}
				else
				{
					INFOLOGC( "inotify_rm_watch ok , inotify_fd[%d] inotify_wd[%d] path_filename[%s]" , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
					
					memset( p_trace_file->path_filename , 0x00 , sizeof(p_trace_file->path_filename) );
					snprintf( p_trace_file->path_filename , sizeof(p_trace_file->path_filename)-1 , "%s/%s" , p_plugin_ctx->path , p_inotify_event->name );
					
					p_trace_file->inotify_file_wd = inotify_add_watch( p_plugin_ctx->inotify_fd , p_trace_file->path_filename , (uint32_t)(IN_MODIFY|IN_CLOSE_WRITE|IN_DELETE_SELF|IN_IGNORED|IN_Q_OVERFLOW) ) ;
					if( p_trace_file->inotify_file_wd == -1 )
					{
						ERRORLOGC( "inotify_add_watch[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
					}
					else
					{
						INFOLOGC( "inotify_add_watch[%s] ok , inotify_fd[%d] inotify_wd[%d] trace_offset[%"PRIu64"] trace_line[%"PRIu64"]" , p_trace_file->path_filename , p_plugin_ctx->inotify_fd , p_trace_file->inotify_file_wd , p_trace_file->trace_offset , p_trace_file->trace_line )
					}
					
					UnlinkTraceFilePathFilenameTreeNode( p_plugin_ctx , p_trace_file );
					nret = LinkTraceFilePathFilenameTreeNode( p_plugin_ctx , p_trace_file ) ;
					if( nret )
					{
						ERRORLOGC( "LinkTraceFilePathFilenameTreeNode failed[%d] , wd[%d] path_filename[%s]" , nret , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
						return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
					}
					else
					{
						DEBUGLOGC( "LinkTraceFilePathFilenameTreeNode ok , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
					}
					
					UnlinkTraceFileInotifyWdTreeNode( p_plugin_ctx , p_trace_file );
					nret = LinkTraceFileInotifyWdTreeNode( p_plugin_ctx , p_trace_file ) ;
					if( nret )
					{
						ERRORLOGC( "LinkTraceFileInotifyWdTreeNode failed[%d] , wd[%d] path_filename[%s]" , nret , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
						return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
					}
					else
					{
						DEBUGLOGC( "LinkTraceFileInotifyWdTreeNode ok , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
					}
				}
			}
			
			INFOLOGC( "free movefrom_filename_node , filename[%s]" , p_curr_movefrom_filename->filename )
			list_del( & (p_curr_movefrom_filename->movefrom_filename_node) );
			free( p_curr_movefrom_filename );
			
			break;
		}
	}
	if( & (p_curr_movefrom_filename->movefrom_filename_node) == & (p_plugin_ctx->movefrom_filename_list) )
	{
		INFOLOGC( "filename[%s] not found in movefrom_filename_list" , p_trace_file->filename )
	}
	
	return 0;
}

static int CleanOldRenameFileEvent( struct InputPluginContext *p_plugin_ctx , struct timeval *p_now )
{
	struct MovefromEvent		*p_curr_movefrom_filename = NULL ;
	struct MovefromEvent		*p_next_movefrom_filename = NULL ;
	
	list_for_each_entry_safe( p_curr_movefrom_filename , p_next_movefrom_filename , & (p_plugin_ctx->movefrom_filename_list) , struct MovefromEvent , movefrom_filename_node )
	{
		DEBUGLOGC( "this_time[%d] p_curr_movefrom_filename->time_to_free[%d]" , p_now->tv_sec , p_curr_movefrom_filename->time_to_free )
		if( p_now->tv_sec > p_curr_movefrom_filename->time_to_free )
		{
			INFOLOGC( "free movefrom_filename_node[%s]" , p_curr_movefrom_filename->filename )
			list_del( & (p_curr_movefrom_filename->movefrom_filename_node) );
			free( p_curr_movefrom_filename );
		}
	}
	
	return 0;
}

static int CleanOldRotatedFile( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , struct InputPluginContext *p_plugin_ctx , struct timeval *p_now )
{
	struct RotatedFile		*p_curr_rotated_file = NULL ;
	struct RotatedFile		*p_next_rotated_file = NULL ;
	struct TraceFile		trace_file ;
	struct TraceFile		*p_trace_file = NULL ;
	
	list_for_each_entry_safe( p_curr_rotated_file , p_next_rotated_file , & (p_plugin_ctx->rotated_file_list) , struct RotatedFile , rotated_file_node )
	{
		DEBUGLOGC( "rotated_file.inotify_file_wd[%d]" , p_curr_rotated_file->inotify_file_wd )
		trace_file.inotify_file_wd = p_curr_rotated_file->inotify_file_wd ;
		p_trace_file = QueryTraceFileInotifyWdTreeNode( p_plugin_ctx , & trace_file ) ;
		if( p_trace_file )
		{
			DEBUGLOGC( "this_time[%d] p_curr_rotated_file->time_to_free[%d]" , p_now->tv_sec , p_trace_file->modifing_timestamp.tv_sec )
			if( p_now->tv_sec - p_trace_file->modifing_timestamp.tv_sec > CLEAN_TIMEOUT_ROTATED_FILE_ELAPSE )
			{
				DEBUGLOGC( "free p_curr_rotated_file[%d]" , p_curr_rotated_file->inotify_file_wd )
				list_del( & (p_curr_rotated_file->rotated_file_node) );
				free( p_curr_rotated_file );
				RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
			}
		}
	}
	
	return 0;
}

funcOnInputPluginIdle OnInputPluginIdle ;
int OnInputPluginIdle( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	struct rb_node			*p = NULL , *p_prev = NULL ;
	struct TraceFile		*p_trace_file = NULL ;
	
	int				nret = 0 ;
	
	gettimeofday( & (p_plugin_ctx->now) , NULL );
	DEBUGLOGC( "now[%ld.%06ld]" , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec )
	
#if 0
	/* 恢复临时调整事件缓冲区 */
	if( p_plugin_ctx->inotify_read_bufsize != INOTIFY_READ_BUFSIZE )
	{
		char	*tmp = NULL ;
		
		WARNLOGC( "inotify read buffer resize [%d]bytes to [%d]bytes" , p_plugin_ctx->inotify_read_bufsize , INOTIFY_READ_BUFSIZE )
		p_plugin_ctx->inotify_read_bufsize = INOTIFY_READ_BUFSIZE ;
		tmp = (char*)realloc( p_plugin_ctx->inotify_read_buffer , p_plugin_ctx->inotify_read_bufsize ) ;
		if( tmp == NULL )
		{
			FATALLOGC( "realloc failed , errno[%d]" , errno )
			return -1;
		}
		p_plugin_ctx->inotify_read_buffer = tmp ;
	}
	else
	{
		INFOLOGC( "input idle" )
	}
#endif
	
	/* 追踪文件变化 */
	p = rb_last( & (p_plugin_ctx->modifing_timestamp_rbtree) );
	while( p )
	{
		p_prev = rb_prev( p ) ;
		
		p_trace_file = container_of( p , struct TraceFile , modifing_timestamp_rbnode ) ;
		DEBUGLOGC( "checking INOTIFY_EVENT IN_MODIFY or IN_CLOSE_WRITE event , now[%ld.%06ld] modifing_timestamp[%ld.%06ld]" , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec , p_trace_file->modifing_timestamp.tv_sec , p_trace_file->modifing_timestamp.tv_usec )
		if( p_plugin_ctx->now.tv_sec > p_trace_file->modifing_timestamp.tv_sec || ( p_plugin_ctx->now.tv_sec == p_trace_file->modifing_timestamp.tv_sec && p_plugin_ctx->now.tv_usec >= p_trace_file->modifing_timestamp.tv_usec ) )
			;
		else
			break;
		
		INFOLOGC( "processing INOTIFY_EVENT IN_MODIFY or IN_CLOSE_WRITE event , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
		
		/* 读取文件追加内容 */
		nret = CheckFileOffset( p_env , p_logpipe_input_plugin, p_plugin_ctx , p_trace_file , p_plugin_ctx->rotate_size , p_plugin_ctx->max_append_count ) ;
		if( nret )
		{
			ERRORLOGC( "CheckFileOffset failed[%d] , path_filename[%s]" , nret , p_trace_file->path_filename )
			return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
		}
		else
		{
			INFOLOGC( "CheckFileOffset ok" )
		}
		
		p = p_prev ;
	}
	
	/* 处理之前保存下来的IN_MOVED_FROM、IN_MOVED_FROM事件，清理没被IN_MOVE_SELF用掉且超时的 */
	nret = CleanOldRenameFileEvent( p_plugin_ctx , & (p_plugin_ctx->now) ) ;
	if( nret )
	{
		ERRORLOGC( "CleanOldRenameFileEvent failed[%d]" , nret )
	}
	else
	{
		DEBUGLOGC( "CleanOldRenameFileEvent ok" )
	}
	
	/* 处理长期不动的大小转档文件 */
	nret = CleanOldRotatedFile( p_env , p_logpipe_input_plugin , p_plugin_ctx , & (p_plugin_ctx->now) ) ;
	if( nret )
	{
		ERRORLOGC( "CleanOldRotatedFile failed[%d]" , nret )
	}
	else
	{
		DEBUGLOGC( "CleanOldRotatedFile ok" )
	}
	
	return 0;
}

funcOnInputPluginEvent OnInputPluginEvent ;
int OnInputPluginEvent( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	
#if 0
	int				inotify_read_nbytes ;
#endif
	long				inotify_read_buflen ;
	long				inotify_event_count ;
	unsigned char			check_all_files_offset_flag ;
	struct inotify_event		*p_inotify_event = NULL ;
	struct inotify_event		*p_overflow_inotify_event = NULL ;
	struct TraceFile		trace_file ;
	struct TraceFile		*p_trace_file = NULL ;
	struct rb_node			*p = NULL ;
	struct rb_node			*p_next = NULL ;
	struct rb_node			*p_prev = NULL ;
	
	static time_t			last_time = 0 ;
	
	static long			inotify_read_buflen_array[ 2 ] = { 0 } ;
	static int			inotify_read_buflen_index = 0 ;
	long				inotify_read_buflen_max ;
	
	int				nret = 0 ;
	
	gettimeofday( & (p_plugin_ctx->now) , NULL );
	DEBUGLOGC( "now[%ld.%06ld]" , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec )
	
#if 0
	/* 如果累积事件数量 大于 默认事件缓冲区大小，临时扩大事件缓冲区大小 */
	nret = ioctl( p_plugin_ctx->inotify_fd , FIONREAD , & inotify_read_nbytes );
	if( nret )
	{
		FATALLOGC( "ioctl failed , errno[%d]" , errno )
		return -1;
	}
	
	if( inotify_read_nbytes+1 > p_plugin_ctx->inotify_read_bufsize )
	{
		char	*tmp = NULL ;
		
		WARNLOGC( "inotify read buffer resize [%d]bytes to [%d]bytes" , p_plugin_ctx->inotify_read_bufsize , inotify_read_nbytes+1 )
		p_plugin_ctx->inotify_read_bufsize = inotify_read_nbytes+1 ;
		tmp = (char*)realloc( p_plugin_ctx->inotify_read_buffer , p_plugin_ctx->inotify_read_bufsize ) ;
		if( tmp == NULL )
		{
			FATALLOGC( "realloc failed , errno[%d]" , errno )
			return -1;
		}
		p_plugin_ctx->inotify_read_buffer = tmp ;
	}
#endif
	
	/* 读文件变化事件 */
	DEBUGLOGC( "read inotify[%d] ..." , p_plugin_ctx->inotify_fd )
	memset( p_plugin_ctx->inotify_read_buffer , 0x00 , p_plugin_ctx->inotify_read_bufsize );
	inotify_read_buflen = read( p_plugin_ctx->inotify_fd , p_plugin_ctx->inotify_read_buffer , p_plugin_ctx->inotify_read_bufsize-1 ) ;
	if( inotify_read_buflen == -1 )
	{
		FATALLOGC( "read inotify[%d] failed , errno[%d]" , p_plugin_ctx->inotify_fd , errno )
		return -1;
	}
	else
	{
		INFOLOGC( "read inotify[%d] ok , [%d]bytes" , p_plugin_ctx->inotify_fd , inotify_read_buflen )
	}
	
	p_inotify_event = (struct inotify_event *)(p_plugin_ctx->inotify_read_buffer) ;
	p_overflow_inotify_event = (struct inotify_event *)(p_plugin_ctx->inotify_read_buffer+inotify_read_buflen) ;
	inotify_event_count = 0 ;
	check_all_files_offset_flag = 0 ;
	while( p_inotify_event < p_overflow_inotify_event )
	{
		/* 如果发生UNMOUNT事件，logpipe退出 */
		if( p_inotify_event->mask & IN_UNMOUNT )
		{
			FATALLOGC( "something unmounted" )
			return -1;
		}
		else if( p_inotify_event->mask & IN_IGNORED )
		{
			;
		}
		else if( p_inotify_event->wd == IN_Q_OVERFLOW )
		{
			/* 如果inotify事件队列溢出，检查所有文件 */
			WARNLOGC( "INOTIFY EVENT QUEUE OVERFLOW!" )
			
			check_all_files_offset_flag = 1 ;
		}
		else if( p_inotify_event->wd == p_plugin_ctx->inotify_path_wd )
		{
			/* 如果发生 创建文件 事件 */
			if( p_inotify_event->mask & IN_CREATE )
			{
				INFOLOGC( "INOTIFY_EVENT IN_CREATE , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name )
				
				nret = AddFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_inotify_event->name , READ_FULL_FILE_FLAG , p_plugin_ctx->rotate_size , p_plugin_ctx->max_append_count ) ;
				if( nret )
				{
					ERRORLOGC( "IN_CREATE AddFileWatcher[%s] failed , errno[%d]" , p_inotify_event->name , errno )
					return -1;
				}
			}
			/* 如果发生 移出文件 事件 */
			else if( p_inotify_event->mask & IN_MOVED_FROM )
			{
				struct MovefromEvent	*p_movefrom_filename = NULL ;
				
				INFOLOGC( "INOTIFY_EVENT IN_MOVED_FROM , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name )
				
				p_movefrom_filename = (struct MovefromEvent *)malloc( sizeof(struct MovefromEvent) ) ;
				if( p_movefrom_filename == NULL )
				{
					ERRORLOGC( "malloc sizeof(struct MovefromEvent) failed , errno[%d]" , errno )
				}
				else
				{
					memset( p_movefrom_filename , 0x00 , sizeof(struct MovefromEvent) ) ;
					strcpy( p_movefrom_filename->filename , p_inotify_event->name );
					p_movefrom_filename->cookie = p_inotify_event->cookie ;
					p_movefrom_filename->time_to_free = time(NULL) + TIME_TO_FREE_ELAPSE ;
					list_add_tail( & (p_movefrom_filename->movefrom_filename_node) , & (p_plugin_ctx->movefrom_filename_list) );
					INFOLOGC( "add movefrom_filename ok , filename[%s] cookie[%d] time_to_free[%d]" , p_movefrom_filename->filename , p_movefrom_filename->cookie , p_movefrom_filename->time_to_free )
				}
			}
			/* 如果发生 移入文件 事件 */
			else if( p_inotify_event->mask & IN_MOVED_TO )
			{
				INFOLOGC( "INOTIFY_EVENT IN_MOVED_TO , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name )
				
				nret = ProcessingRenameFileEvent( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_inotify_event ) ;
				if( nret )
				{
					ERRORLOGC( "ProcessingRenameFileEvent failed[%d]" , nret )
				}
				else
				{
					DEBUGLOGC( "ProcessingRenameFileEvent ok" )
				}
			}
			/* 如果发生 删除文件 事件 */
			else if( p_inotify_event->mask & IN_DELETE )
			{
				INFOLOGC( "INOTIFY_EVENT IN_DELETE , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name )
				
				snprintf( trace_file.path_filename , sizeof(trace_file.path_filename)-1 , "%s/%s" , p_plugin_ctx->path , p_inotify_event->name );
				p_trace_file = QueryTraceFilePathFilenameTreeNode( p_plugin_ctx , & trace_file ) ;
				if( p_trace_file )
				{
					/* 采集完文件全部追加内容 */
					nret = CheckFileOffset( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file , 0 , APPEND_COUNT_INFINITED ) ;
					if( nret )
					{
						ERRORLOGC( "CheckFileOffset failed[%d] , errno[%d] path_filename[%s]" , nret , errno , p_trace_file->path_filename )
					}
					else
					{
						INFOLOGC( "CheckFileOffset ok , path_filename[%s]" , p_trace_file->path_filename )
					}
					
					/* 清除原文件 */
					INFOLOGC( "RemoveFileWatcher ... path_filename[%s]" , p_trace_file->path_filename )
					nret = RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file ) ;
					if( nret )
					{
						ERRORLOGC( "RemoveFileWatcher failed[%d] , errno[%d] path_filename[%s]" , nret , errno , p_trace_file->path_filename )
						return -1;
					}
				}
			}
			/* 如果发生 删除监控目录 或 移动监控目录 事件 */
			else if( ( p_inotify_event->mask & IN_DELETE_SELF ) || ( p_inotify_event->mask & IN_MOVE_SELF ) )
			{
				ERRORLOGC( "[%s] had deleted or moved out" , p_plugin_ctx->path )
				ERRORLOGC( "inotify_rm_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_plugin_ctx->path , p_plugin_ctx->inotify_fd , p_plugin_ctx->inotify_path_wd )
				inotify_rm_watch( p_plugin_ctx->inotify_fd , p_plugin_ctx->inotify_path_wd );
				return -1;
			}
			else
			{
				ERRORLOGC( "unknow dir inotify event mask[0x%X]" , p_inotify_event->mask )
			}
		}
		else
		{
			/* 查询文件跟踪结构树 */
			trace_file.inotify_file_wd = p_inotify_event->wd ;
			p_trace_file = QueryTraceFileInotifyWdTreeNode( p_plugin_ctx , & trace_file ) ;
			if( p_trace_file == NULL )
			{
				/*
				WARNLOGC( "wd[%d] not found" , trace_file.inotify_file_wd )
				*/
			}
			else
			{
				/* 如果发生 文件修改 事件 */
				if( p_inotify_event->mask & IN_MODIFY )
				{
					DEBUGLOGC( "INOTIFY_EVENT IN_MODIFY , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s] , now[%ld.%06ld]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec )
					
					/* 合并处理文件变动事件 */
					UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
					p_trace_file->modifing_timestamp.tv_sec = p_plugin_ctx->now.tv_sec ;
					p_trace_file->modifing_timestamp.tv_usec = p_plugin_ctx->now.tv_usec ;
					LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
				}
				/* 如果发生 写完关闭 事件 */
				else if( p_inotify_event->mask & IN_CLOSE_WRITE )
				{
					DEBUGLOGC( "INOTIFY_EVENT IN_CLOSE_WRITE , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s] , now[%ld.%06ld]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec )
					
					/* 合并处理文件变动事件 */
					UnlinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
					p_trace_file->modifing_timestamp.tv_sec = p_plugin_ctx->now.tv_sec ;
					p_trace_file->modifing_timestamp.tv_usec = p_plugin_ctx->now.tv_usec ;
					LinkTraceFileModifingTimestampTreeNode( p_plugin_ctx , p_trace_file );
				}
				/* 如果发生 删除文件 事件 */
				else if( p_inotify_event->mask & IN_DELETE_SELF )
				{
					DEBUGLOGC( "INOTIFY_EVENT IN_DELETE_SELF , wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s] , now[%ld.%06ld]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec )
					
					/* 采集完文件全部追加内容 */
					nret = CheckFileOffset( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file , 0 , APPEND_COUNT_INFINITED ) ;
					if( nret )
					{
						ERRORLOGC( "CheckFileOffset failed[%d] , errno[%d] path_filename[%s]" , nret , errno , p_trace_file->path_filename )
					}
					else
					{
						INFOLOGC( "CheckFileOffset ok , path_filename[%s]" , p_trace_file->path_filename )
					}
					
					/* 清除原文件 */
					INFOLOGC( "RemoveFileWatcher ... path_filename[%s]" , p_trace_file->path_filename )
					nret = RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file ) ;
					if( nret )
					{
						ERRORLOGC( "RemoveFileWatcher failed[%d] , errno[%d] path_filename[%s]" , nret , errno , p_trace_file->path_filename )
						return -1;
					}
				}
				else
				{
					ERRORLOGC( "unknow file inotify event mask[0x%X]" , p_inotify_event->mask )
				}
			}
		}
		
		p_inotify_event = (struct inotify_event *)( (char*)p_inotify_event + sizeof(struct inotify_event) + p_inotify_event->len ) ;
		inotify_event_count++;
	}
	
	INFOLOGC( "[%ld]bytes [%ld]inotify events processed" , inotify_read_buflen , inotify_event_count )
	
	/* 如果inotify缓冲区溢出，检查所有文件 */
	if( INOTIFY_READ_BUFSIZE-inotify_read_buflen<sizeof(struct inotify_event)+PATH_MAX+1 )
	{
		WARNLOGC( "INOTIFY BUFFER OVERFLOW!" )
		
		check_all_files_offset_flag = 1 ;
	}
	
	/* 处理文件合并变动事件 */
	if( check_all_files_offset_flag )
	{
		p = rb_first( & (p_plugin_ctx->inotify_wd_rbtree) );
		while( p )
		{
			p_next = rb_next( p ) ;
			
			p_trace_file = container_of( p , struct TraceFile , inotify_file_wd_rbnode ) ;
			
			/* 读取文件所有追加内容 */
			nret = CheckFileOffset( p_env , p_logpipe_input_plugin, p_plugin_ctx , p_trace_file , 0 , APPEND_COUNT_INFINITED ) ;
			if( nret )
			{
				ERRORLOGC( "CheckFileOffset failed[%d] , path_filename[%s]" , nret , p_trace_file->path_filename )
				return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
			}
			else
			{
				INFOLOGC( "CheckFileOffset ok , path_filename[%s]" , p_trace_file->path_filename )
			}
			
			p = p_next ;
		}
		
		check_all_files_offset_flag = 0 ;
	}
	else
	{
		p = rb_last( & (p_plugin_ctx->modifing_timestamp_rbtree) );
		while( p )
		{
			p_prev = rb_prev( p ) ;
			
			p_trace_file = container_of( p , struct TraceFile , modifing_timestamp_rbnode ) ;
			DEBUGLOGC( "checking INOTIFY_EVENT IN_MODIFY or IN_CLOSE_WRITE event , now[%ld.%06ld] modifing_timestamp[%ld.%06ld]" , p_plugin_ctx->now.tv_sec , p_plugin_ctx->now.tv_usec , p_trace_file->modifing_timestamp.tv_sec , p_trace_file->modifing_timestamp.tv_usec )
			if( p_plugin_ctx->now.tv_sec > p_trace_file->modifing_timestamp.tv_sec || ( p_plugin_ctx->now.tv_sec == p_trace_file->modifing_timestamp.tv_sec && p_plugin_ctx->now.tv_usec >= p_trace_file->modifing_timestamp.tv_usec ) )
				;
			else
				break;
			
			INFOLOGC( "processing INOTIFY_EVENT IN_MODIFY or IN_CLOSE_WRITE event , wd[%d] path_filename[%s]" , p_trace_file->inotify_file_wd , p_trace_file->path_filename )
			
			/* 读取文件追加内容 */
			nret = CheckFileOffset( p_env , p_logpipe_input_plugin, p_plugin_ctx , p_trace_file , p_plugin_ctx->rotate_size , p_plugin_ctx->max_append_count ) ;
			if( nret )
			{
				ERRORLOGC( "CheckFileOffset failed[%d] , path_filename[%s]" , nret , p_trace_file->path_filename )
				return RemoveFileWatcher( p_env , p_logpipe_input_plugin , p_plugin_ctx , p_trace_file );
			}
			else
			{
				INFOLOGC( "CheckFileOffset ok" )
			}
			
			p = p_prev ;
		}
	}
	
	/* 每隔一秒 */
	if( p_plugin_ctx->now.tv_sec > last_time )
	{
		/* 处理之前保存下来的IN_MOVED_FROM、IN_MOVED_FROM事件，清理没被IN_MOVE_SELF消耗掉且超时的 */
		nret = CleanOldRenameFileEvent( p_plugin_ctx , & (p_plugin_ctx->now) ) ;
		if( nret )
		{
			ERRORLOGC( "CleanOldRenameFileEvent failed[%d]" , nret )
		}
		else
		{
			DEBUGLOGC( "CleanOldRenameFileEvent ok" )
		}
		
		/* 处理长期不动的大小转档文件 */
		nret = CleanOldRotatedFile( p_env , p_logpipe_input_plugin , p_plugin_ctx , & (p_plugin_ctx->now) ) ;
		if( nret )
		{
			ERRORLOGC( "CleanOldRenameFileEvent failed[%d]" , nret )
		}
		else
		{
			DEBUGLOGC( "CleanOldRenameFileEvent ok" )
		}
		
		last_time = p_plugin_ctx->now.tv_sec + 5 ;
	}
	
	/* 沉睡一段时间，降低CPU耗用 */
	if( p_plugin_ctx->max_usleep_interval > 0 )
	{
		uint64_t	usleep_interval ;
		
		inotify_read_buflen_array[ inotify_read_buflen_index ] = inotify_read_buflen ;
		inotify_read_buflen_max = MAX( inotify_read_buflen_array[0] , inotify_read_buflen_array[1] ) ;
		usleep_interval = (uint64_t)(((double)(INOTIFY_READ_BUFSIZE-inotify_read_buflen_max)/(double)(INOTIFY_READ_BUFSIZE))*p_plugin_ctx->max_usleep_interval) ;
		if( p_plugin_ctx->min_usleep_interval > 0 && usleep_interval < p_plugin_ctx->min_usleep_interval )
			usleep_interval = p_plugin_ctx->min_usleep_interval ;
		
		INFOLOGC( "inotify_read_buflen_array[%d][%d] - inotify_read_buflen_max[%d] - usleep(%"PRIu64")" , inotify_read_buflen_index , inotify_read_buflen_array[inotify_read_buflen_index] , inotify_read_buflen_max , usleep_interval )
		if( usleep_interval > 0 )
		{
			usleep( usleep_interval );
		}
		
		inotify_read_buflen_index = 1 - inotify_read_buflen_index ;
	}
	
	return 0;
}

funcBeforeReadInputPlugin BeforeReadInputPlugin ;
int BeforeReadInputPlugin( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context , uint64_t *p_file_offset , uint64_t *p_file_line )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	struct TraceFile		*p_trace_file = p_plugin_ctx->p_trace_file ;
	
	(*p_file_offset) = p_trace_file->trace_offset ;
	(*p_file_line) = p_trace_file->trace_line ;
	INFOLOGC( "BeforeReadInputPlugin[%s] file_offset[%"PRIu64"] file_line[%"PRIu64"]" , p_trace_file->path_filename , (*p_file_offset) , (*p_file_line) )
	p_plugin_ctx->add_len = 0 ;
	p_plugin_ctx->add_line = 0 ;
	
	return 0;
}

funcReadInputPlugin ReadInputPlugin ;
int ReadInputPlugin( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context , uint64_t *p_file_offset , uint64_t *p_file_line , uint64_t *p_block_len , char *block_buf , uint64_t block_bufsize )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	struct TraceFile		*p_trace_file = p_plugin_ctx->p_trace_file ;
	
	uint64_t			read_len = 0 ;
	
	int				nret = 0 ;
	
	if( p_plugin_ctx->remain_len == 0 )
		return LOGPIPE_READ_END_OF_INPUT;
	
	/* 如果未启用压缩 */
	if( p_plugin_ctx->compress_algorithm == NULL )
	{
		if( p_plugin_ctx->remain_len > block_bufsize - 1 )
			read_len = block_bufsize - 1 ;
		else
			read_len = p_plugin_ctx->remain_len ;
		memset( block_buf , 0x00 , read_len+1 );
		p_plugin_ctx->read_len = read( p_plugin_ctx->fd , block_buf , read_len ) ;
		if( p_plugin_ctx->read_len == -1 )
		{
			ERRORLOGC( "read file failed , errno[%d]" , errno )
			return -1;
		}
		else if( p_plugin_ctx->read_len == 0 )
		{
			WARNLOGC( "read eof of file" )
			return LOGPIPE_READ_END_OF_INPUT;
		}
		else
		{
			INFOLOGC( "read file ok , [%"PRIu64"]bytes" , p_plugin_ctx->read_len )
			DEBUGHEXLOGC( block_buf , p_plugin_ctx->read_len , NULL )
		}
		
		p_plugin_ctx->read_line = stat_memchr( block_buf , p_plugin_ctx->read_len , '\n' ) ;
		
		(*p_block_len) = p_plugin_ctx->read_len ;
	}
	/* 如果启用了压缩 */
	else
	{
		char			block_in_buf[ LOGPIPE_UNCOMPRESS_BLOCK_BUFSIZE + 1 ] ;
		uint64_t		block_in_len ;
		
		if( p_plugin_ctx->remain_len > sizeof(block_in_buf) - 1 )
			block_in_len = sizeof(block_in_buf) - 1 ;
		else
			block_in_len = p_plugin_ctx->remain_len ;
		
		memset( block_in_buf , 0x00 , block_in_len+1 );
		p_plugin_ctx->read_len = read( p_plugin_ctx->fd , block_in_buf , block_in_len ) ;
		if( p_plugin_ctx->read_len == -1 )
		{
			ERRORLOGC( "read file failed , errno[%d]" , errno )
			return -1;
		}
		else
		{
			INFOLOGC( "read file ok , [%"PRIu64"]bytes" , p_plugin_ctx->read_len )
			DEBUGHEXLOGC( block_in_buf , p_plugin_ctx->read_len , NULL )
		}
		
		p_plugin_ctx->read_line = stat_memchr( block_buf , p_plugin_ctx->read_len , '\n' ) ;
		
		memset( block_buf , 0x00 , block_bufsize );
		nret = CompressInputPluginData( p_plugin_ctx->compress_algorithm , block_in_buf , p_plugin_ctx->read_len , block_buf , p_block_len ) ;
		if( nret )
		{
			ERRORLOGC( "CompressInputPluginData failed[%d]" , nret )
			return -1;
		}
		else
		{
			DEBUGLOGC( "CompressInputPluginData ok" )
		}
	}
	
	p_plugin_ctx->remain_len -= p_plugin_ctx->read_len ;
	
	(*p_file_offset) += p_plugin_ctx->read_len ;
	(*p_file_line) += p_plugin_ctx->read_line ;
	INFOLOGC( "ReadInputPlugin[%s] - file_offset[%"PRIu64"]+=[%"PRIu64"] file_line[%"PRIu64"]+=[%"PRIu64"] remain_len[%"PRIu64"]-=[%"PRIu64"]" , p_trace_file->path_filename , (*p_file_offset) , p_plugin_ctx->read_len , (*p_file_line) , p_plugin_ctx->read_line , p_plugin_ctx->remain_len , read_len )
	p_plugin_ctx->add_len += p_plugin_ctx->read_len ;
	p_plugin_ctx->add_line += p_plugin_ctx->read_line ;
	
	return 0;
}

funcAfterReadInputPlugin AfterReadInputPlugin ;
int AfterReadInputPlugin( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context , uint64_t *p_file_offset , uint64_t *p_file_line )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	struct TraceFile		*p_trace_file = p_plugin_ctx->p_trace_file ;
	
	p_trace_file->trace_offset = (*p_file_offset) ;
	p_trace_file->trace_line = (*p_file_line) ;
	INFOLOGC( "AfterReadInputPlugin[%s] - trace_offset[%"PRIu64"]+=[%"PRIu64"] trace_line[%"PRIu64"]+=[%"PRIu64"]" , p_trace_file->path_filename , p_plugin_ctx->p_trace_file->trace_offset , p_plugin_ctx->add_len , p_trace_file->trace_line , p_plugin_ctx->add_line )
	
	return 0;
}

funcCleanInputPluginContext CleanInputPluginContext ;
int CleanInputPluginContext( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void *p_context )
{
	struct InputPluginContext	*p_plugin_ctx = (struct InputPluginContext *)p_context ;
	struct MovefromEvent		*p_curr_movefrom_filename = NULL ;
	struct MovefromEvent		*p_next_movefrom_filename = NULL ;
	struct RotatedFile		*p_curr_rotated_file = NULL ;
	struct RotatedFile		*p_next_rotated_file = NULL ;
	
	list_for_each_entry_safe( p_curr_movefrom_filename , p_next_movefrom_filename , & (p_plugin_ctx->movefrom_filename_list) , struct MovefromEvent , movefrom_filename_node )
	{
		free( p_curr_movefrom_filename );
	}
	list_for_each_entry_safe( p_curr_rotated_file , p_next_rotated_file , & (p_plugin_ctx->rotated_file_list) , struct RotatedFile , rotated_file_node )
	{
		free( p_curr_rotated_file );
	}
	
	inotify_rm_watch( p_plugin_ctx->inotify_fd , p_plugin_ctx->inotify_path_wd );
	close( p_plugin_ctx->inotify_fd );
	DestroyTraceFileTree( p_plugin_ctx );
	
	free( p_plugin_ctx->inotify_read_buffer );
	
	return 0;
}

funcUnloadInputPluginConfig UnloadInputPluginConfig ;
int UnloadInputPluginConfig( struct LogpipeEnv *p_env , struct LogpipeInputPlugin *p_logpipe_input_plugin , void **pp_context )
{
	struct InputPluginContext	**pp_plugin_ctx = (struct InputPluginContext **)pp_context ;
	
	/* 释放内存以存放插件上下文 */
	free( (*pp_plugin_ctx) ); (*pp_plugin_ctx) = NULL ;
	
	return 0;
}

