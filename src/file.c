#include "logpipe_in.h"

int AddFileWatcher( struct InotifySession *p_inotify_session , char *filename )
{
	struct TraceFile	*p_trace_file = NULL ;
	struct TraceFile	*p_trace_file_not_exist = NULL ;
	struct stat		file_stat ;
	static uint32_t		inotify_mask = IN_CLOSE_WRITE|IN_DELETE_SELF|IN_MOVE_SELF|IN_IGNORED ;
	
	int			nret = 0 ;
	
	if( filename[0] == '.' )
	{
		INFOLOG( "file[%s] has ignored" , filename )
		return 0;
	}
	
	p_trace_file = (struct TraceFile *)malloc( sizeof(struct TraceFile) ) ;
	if( p_trace_file == NULL )
	{
		ERRORLOG( "malloc failed , errno[%d]" , errno )
		return -1;
	}
	memset( p_trace_file , 0x00 , sizeof(struct TraceFile) );
	
	p_trace_file->path_filename_len = asprintf( & (p_trace_file->path_filename) , "%s/%s" , p_inotify_session->inotify_path , filename );
	if( p_trace_file->path_filename_len == -1 )
	{
		ERRORLOG( "asprintf failed , errno[%d]" , errno )
		free( p_trace_file );
		return -1;
	}
	
	nret = stat( p_trace_file->path_filename , & file_stat ) ;
	if( nret == -1 )
	{
		WARNLOG( "file[%s] not found" , p_trace_file->path_filename )
		free( p_trace_file->path_filename );
		free( p_trace_file );
		return 0;
	}
	
	p_trace_file->filename_len = asprintf( & p_trace_file->filename , "%s" , filename );
	if( p_trace_file->filename_len == -1 )
	{
		ERRORLOG( "asprintf failed , errno[%d]" , errno )
		free( p_trace_file->path_filename );
		free( p_trace_file );
		return -1;
	}
	
	p_trace_file->inotify_file_wd = inotify_add_watch( p_inotify_session->inotify_fd , p_trace_file->path_filename , inotify_mask ) ;
	if( p_trace_file->inotify_file_wd == -1 )
	{
		ERRORLOG( "inotify_add_watch[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		free( p_trace_file->path_filename );
		free( p_trace_file );
		return -1;
	}
	else
	{
		INFOLOG( "inotify_add_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_trace_file->path_filename , p_inotify_session->inotify_fd , p_trace_file->inotify_file_wd )
	}
	
	p_trace_file_not_exist = QueryTraceFileWdTreeNode( p_inotify_session , p_trace_file ) ;
	if( p_trace_file_not_exist == NULL )
	{
		nret = LinkTraceFileWdTreeNode( p_inotify_session , p_trace_file ) ;
		if( nret )
		{
			ERRORLOG( "LinkTraceFileWdTreeNode[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
			INFOLOG( "inotify_rm_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_trace_file->path_filename , p_inotify_session->inotify_fd , p_trace_file->inotify_file_wd )
			inotify_rm_watch( p_inotify_session->inotify_fd , p_trace_file->inotify_file_wd );
			free( p_trace_file->path_filename );
			free( p_trace_file );
			return -1;
		}
	}
	
	return 0;
}

int RemoveFileWatcher( struct InotifySession *p_inotify_session , struct TraceFile *p_trace_file )
{
	UnlinkTraceFileWdTreeNode( p_inotify_session , p_trace_file );
	
	INFOLOG( "inotify_rm_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_trace_file->path_filename , p_inotify_session->inotify_fd , p_trace_file->inotify_file_wd )
	inotify_rm_watch( p_inotify_session->inotify_fd , p_trace_file->inotify_file_wd );
	free( p_trace_file->path_filename );
	free( p_trace_file );
	
	return 0;
}

static int OutputFileAppender( struct LogPipeEnv *p_env , struct InotifySession *p_inotify_session , struct TraceFile *p_trace_file )
{
	int		fd ;
	struct stat	file_stat ;
	int		appender_len ;
	
	int		nret = 0 ;
	
	DEBUGLOG( "catch file[%s] appender" , p_trace_file->path_filename )
	
	fd = open( p_trace_file->path_filename , O_RDONLY ) ;
	if( fd == -1 )
	{
		ERRORLOG( "open[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		return -1;
	}
	
	memset( & file_stat , 0x00 , sizeof(struct stat) );
	nret = fstat( fd , & file_stat ) ;
	if( nret == -1 )
	{
		ERRORLOG( "fstat[%s] failed , errno[%d]" , p_trace_file->path_filename , errno )
		close( fd );
		return RemoveFileWatcher( p_inotify_session , p_trace_file );
	}
	
	DEBUGLOG( "file_size[%d] trace_offset[%d]" , file_stat.st_size , p_trace_file->trace_offset )
	if( file_stat.st_size < p_trace_file->trace_offset )
	{
		p_trace_file->trace_offset = file_stat.st_size ;
	}
	else if( file_stat.st_size > p_trace_file->trace_offset )
	{
		appender_len = file_stat.st_size - p_trace_file->trace_offset ;
		
		nret = FileToOutputs( p_env , p_trace_file , fd , appender_len ) ;
		if( nret )
		{
			ERRORLOG( "FileToOutputs failed[%d]" , nret )
			close( fd );
			return nret;
		}
		
		p_trace_file->trace_offset = file_stat.st_size ;
	}
	
	close( fd );
	
	return 0;
}

int OnInotifyHandler( struct LogPipeEnv *p_env , struct InotifySession *p_inotify_session )
{
	long			read_len ;
	struct inotify_event	*p_inotify_event = NULL ;
	struct inotify_event	*p_overflow_inotify_event = NULL ;
	struct TraceFile	trace_file ;
	struct TraceFile	*p_trace_file = NULL ;
	
	int			nret = 0 ;
	
	DEBUGLOG( "read inotify ..." )
	read_len = read( p_inotify_session->inotify_fd , p_env->inotify_read_buffer , sizeof(p_env->inotify_read_buffer)-1 ) ;
	if( read_len == -1 )
	{
		ERRORLOG( "read failed , errno[%d]" , errno )
		return -1;
	}
	else
	{
		INFOLOG( "read inotify ok , [%d]bytes" , read_len )
	}
	
	p_inotify_event = (struct inotify_event *)(p_env->inotify_read_buffer) ;
	p_overflow_inotify_event = (struct inotify_event *)(p_env->inotify_read_buffer+read_len) ;
	while( p_inotify_event < p_overflow_inotify_event )
	{
		DEBUGLOG( "inotify event wd[%d] mask[0x%X] cookie[%d] len[%d] name[%.*s]" , p_inotify_event->wd , p_inotify_event->mask , p_inotify_event->cookie , p_inotify_event->len , p_inotify_event->len , p_inotify_event->name )
		
		if( p_inotify_event->mask & IN_IGNORED )
		{
			;
		}
		else if( p_inotify_event->wd == p_inotify_session->inotify_path_wd )
		{
			if( ( p_inotify_event->mask & IN_CREATE ) || ( p_inotify_event->mask & IN_MOVED_TO ) )
			{
				nret = AddFileWatcher( p_inotify_session , p_inotify_event->name ) ;
				if( nret )
				{
					ERRORLOG( "AddFileWatcher[%s] failed , errno[%d]" , p_inotify_event->name , errno )
					return -1;
				}
			}
			else if( ( p_inotify_event->mask & IN_DELETE_SELF ) || ( p_inotify_event->mask & IN_MOVE_SELF ) )
			{
				INFOLOG( "[%s] had deleted" , p_inotify_session->inotify_path )
				INFOLOG( "inotify_rm_watch[%s] ok , inotify_fd[%d] inotify_wd[%d]" , p_inotify_session->inotify_path , p_inotify_session->inotify_fd , p_inotify_session->inotify_path_wd )
				inotify_rm_watch( p_inotify_session->inotify_fd , p_inotify_session->inotify_path_wd );
				break;
			}
			else
			{
				ERRORLOG( "unknow dir inotify event mask[0x%X]" , p_inotify_event->mask )
			}
		}
		else
		{
			trace_file.inotify_file_wd = p_inotify_event->wd ;
			p_trace_file = QueryTraceFileWdTreeNode( p_inotify_session , & trace_file ) ;
			if( p_trace_file == NULL )
			{
				ERRORLOG( "wd[%d] not found" , trace_file.inotify_file_wd )
			}
			else
			{
				if( p_inotify_event->mask & IN_CLOSE_WRITE )
				{
					nret = OutputFileAppender( p_env , p_inotify_session , p_trace_file ) ;
					if( nret )
					{
						ERRORLOG( "OutputFileAppender failed , errno[%d]" , errno )
						return -1;
					}
				}
				else if( ( p_inotify_event->mask & IN_DELETE_SELF ) || ( p_inotify_event->mask & IN_MOVE_SELF ) )
				{
					nret = RemoveFileWatcher( p_inotify_session , p_trace_file ) ;
					if( nret )
					{
						ERRORLOG( "RemoveFileWatcher failed , errno[%d]" , errno )
						return -1;
					}
				}
				else
				{
					ERRORLOG( "unknow file inotify event mask[0x%X]" , p_inotify_event->mask )
				}
			}
		}
		
		p_inotify_event = (struct inotify_event *)( (char*)p_inotify_event + sizeof(struct inotify_event) + p_inotify_event->len ) ;
	}
	
	return 0;
}

#if 0

/* 落地新增日志 */
int OnDumpLog( struct LogPipeEnv *p_env , struct AcceptedSession *p_accepted_session )
{
	char		*p = NULL ;
	uint32_t	*file_name_len = NULL ;
	uint32_t	file_name_len_ntohl ;
	char		path_filename[ PATH_MAX + 1 ] ;
	uint32_t	file_data_len ;
	int		fd ;
	
	int		nret = 0 ;
	
	p = p_accepted_session->comm_buf + sizeof(uint32_t) ;
	
	DEBUGHEXLOG( p , 1 , "magic" )
	if( (*p) != LOGPIPE_COMM_MAGIC[0] )
	{
		ERRORLOG( "comm magic[%d][%c] not match" , (*p) , (*p) )
		return -1;
	}
	
	p++;
	
	file_name_len = (uint32_t*)p ;
	file_name_len_ntohl = ntohl( (*file_name_len) ) ;
	DEBUGHEXLOG( p , sizeof(uint32_t) , "file name len[%d]" , file_name_len_ntohl )
	
	p += sizeof(uint32_t) ;
	
	memset( path_filename , 0x00 , sizeof(path_filename) );
	snprintf( path_filename , sizeof(path_filename)-1 , "%s/%.*s" , p_env->role_context.dumpserver.dump_path , file_name_len_ntohl , p );
	fd = open( path_filename , O_CREAT|O_WRONLY|O_APPEND , 00777 ) ;
	if( fd == -1 )
	{
		ERRORLOG( "open file[%s] failed , errno[%d]" , path_filename , errno )
		return -1;
	}
	else
	{
		DEBUGLOG( "open file[%s] ok" , path_filename )
	}
	
	p += file_name_len_ntohl ;
	
	file_data_len = p_accepted_session->comm_body_len - 1 - sizeof(file_name_len_ntohl) - file_name_len_ntohl ;
	nret = write( fd , p , file_data_len ) ;
	DEBUGHEXLOG( p , file_data_len , "write file[%s] [%d]bytes return[%d]" , path_filename , file_data_len , nret )
	
	close( fd );
	DEBUGLOG( "close file[%s] ok" , path_filename )
	
	return 0;
}

#endif
