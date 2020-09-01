abs_bindir=$1
abs_srcdir=$2
abs_port=$3
dataNode=$4
x_option=$5
format=$6
# backup
$abs_bindir/gs_basebackup -D $abs_bindir/../$dataNode -p $abs_port -X$x_option -F$format > $abs_bindir/../$dataNode.log 2>&1
for gs_basebackup_port in {40015..60000}; 
do 
    if [ 'x'`netstat -an | grep -v STREAM | grep -v DGRAM | grep $gs_basebackup_port | head -n1 | awk '{print $1}'` == 'x' ]; 
    then  
        break; 
    fi; 
done; 

if [ $format == 't' ] 
then
    tmp_dir="$abs_bindir/../$dataNode/../tmp"
    mv $abs_bindir/../$dataNode/* $abs_bindir/../$dataNode/../
    $abs_bindir/gs_tar -F $abs_bindir/../base.tar -D $abs_bindir/../$dataNode/
    mkdir $abs_bindir/../$dataNode/pg_location
    count='0';
    tablespace="";
    mkdir $tmp_dir
    absolute_path=`cd $abs_bindir; pwd`
    for i in `cat $abs_bindir/../$dataNode/tablespace_map`;
    do
        if [ $count == '0' ];
        then
            tablespace=$i;
            count='1';
        else
            mkdir "$abs_bindir/../$dataNode/pg_location/${i##/*/}"
            $abs_bindir/gs_tar -F $abs_bindir/../$tablespace.tar -D $tmp_dir
            mv $tmp_dir/* "$abs_bindir/../$dataNode/pg_location/${i##/*/}"
            count='0';
        fi
    done
fi


$abs_bindir/gs_ctl start -o "-p ${gs_basebackup_port} -c listen_addresses=*" -D $abs_bindir/../$dataNode >> $abs_bindir/../$dataNode.log 2>&1


# ----check start or not
$abs_bindir/gs_ctl status -D $abs_bindir/../$dataNode

#validate
$abs_bindir/gsql -dgs_basebackup -p$gs_basebackup_port -f "$abs_srcdir/sql/gs_basebackup/validate/tablespace.sql";
$abs_bindir/gsql -dgs_basebackup -p$gs_basebackup_port -f "$abs_srcdir/sql/gs_basebackup/validate/mot.sql";


#stop node
$abs_bindir/gs_ctl stop -D $abs_bindir/../$dataNode